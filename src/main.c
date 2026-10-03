/* main.c - duckyc, the Ducky language compiler.
 *
 * Pipeline: source -> tokens -> AST -> type-checked AST -> x86-64 assembly
 * -> object file (GNU as) -> executable (system linker).
 */
#define _XOPEN_SOURCE 700      /* realpath */
#define _POSIX_C_SOURCE 200809L /* mkdtemp, fork, execvp, waitpid */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "codegen.h"
#include "common.h"
#include "lexer.h"
#include "parser.h"
#include "sema.h"
#include "version.h"

typedef struct {
    const char *input;
    const char *output;
    int emit_asm;
    int dump_tokens;
} Options;

static void usage(FILE *f) {
    fprintf(f,
            "duckyc - the %s language compiler\n"
            "\n"
            "Usage: duckyc [options] <file.duck>\n"
            "\n"
            "Options:\n"
            "  -o <path>        write the output to <path>\n"
            "  -S, --emit-asm   stop after writing x86-64 assembly\n"
            "  --dump-tokens    show the token stream and exit\n"
            "  -h, --help       show this help and exit\n"
            "  --version        show version information and exit\n"
            "\n"
            "Without -o, the output name is derived from the input file:\n"
            "  duckyc hello.duck   -> ./hello\n"
            "  duckyc -S hello.duck -> ./hello.s\n",
            DUCKY_LANG_NAME);
}

/* Run a child process and return its exit status. */
static int run(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) fatal("cannot fork: %s", strerror(errno));
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "duckyc: error: cannot run '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) fatal("cannot wait for '%s': %s", argv[0], strerror(errno));
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}

/* "dir/name.duck" + ".s" -> "dir/name.s" */
static char *derive_path(const char *input, const char *ext) {
    const char *dot = strrchr(input, '.');
    const char *slash = strrchr(input, '/');
    size_t base = strlen(input);
    if (dot && (!slash || dot > slash) && strcmp(dot, ".duck") == 0) {
        base = (size_t)(dot - input);
    }
    char *out = malloc(base + strlen(ext) + 1);
    if (!out) fatal("out of memory");
    memcpy(out, input, base);
    strcpy(out + base, ext);
    return out;
}

static Options parse_args(int argc, char **argv) {
    Options o;
    memset(&o, 0, sizeof(o));

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-o") == 0) {
            if (i + 1 >= argc) fatal("option '-o' requires an argument");
            o.output = argv[++i];
        } else if (strcmp(a, "-S") == 0 || strcmp(a, "--emit-asm") == 0) {
            o.emit_asm = 1;
        } else if (strcmp(a, "--dump-tokens") == 0) {
            o.dump_tokens = 1;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            exit(0);
        } else if (strcmp(a, "--version") == 0) {
            printf("duckyc %s (the %s language compiler)\n", DUCKYC_VERSION, DUCKY_LANG_NAME);
            exit(0);
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "duckyc: error: unknown option '%s'\n", a);
            usage(stderr);
            exit(1);
        } else {
            if (o.input) fatal("only one input file is supported (got '%s' and '%s')", o.input, a);
            o.input = a;
        }
    }
    return o;
}

static int compile_to_assembly(const SourceFile *src, Program *prog, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) fatal("cannot write '%s': %s", path, strerror(errno));
    generate(src, prog, f);
    if (fclose(f) != 0) fatal("cannot write '%s': %s", path, strerror(errno));
    return 0;
}

/* ---------- multi-file programs ------------------------------------------
 * `import "path.duck";` pulls another source file into the program. The
 * driver loads the whole import graph depth first, each file at most once
 * (keyed by its canonical path, so diamonds and cycles are fine), interns
 * the struct names of every file, parses each file with its own token
 * stream (diagnostics then name that file) and merges all declarations into
 * one program that is analyzed and compiled as a single unit. */

typedef struct {
    const SourceFile *src;
    Token *toks;
    int ntoks;
    char *dir; /* directory containing this file */
} Unit;

static Unit *g_units;
static int g_nunits;
static int g_units_cap;

typedef struct SeenPath {
    const char *path;
    struct SeenPath *next;
} SeenPath;

static SeenPath *g_seen;

static int seen(const char *key) {
    for (SeenPath *s = g_seen; s; s = s->next) {
        if (strcmp(s->path, key) == 0) return 1;
    }
    return 0;
}

static void mark_seen(const char *key) {
    SeenPath *s = arena_alloc(sizeof(*s));
    s->path = arena_strndup(key, strlen(key));
    s->next = g_seen;
    g_seen = s;
}

/* Directory part of a path; "x.duck" -> ".", "lib/x.duck" -> "lib". */
static char *dir_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return arena_strndup(".", 1);
    if (slash == path) return arena_strndup("/", 1);
    return arena_strndup(path, (size_t)(slash - path));
}

/* Resolve `name` against the directory of the importing file. */
static char *join_path(const char *dir, const char *name) {
    if (name[0] == '/') return arena_strndup(name, strlen(name));
    size_t dl = strlen(dir), nl = strlen(name);
    if (dl == 1 && dir[0] == '.') {
        char *out = arena_alloc(nl + 1);
        memcpy(out, name, nl + 1);
        return out;
    }
    char *out = arena_alloc(dl + nl + 2);
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1, name, nl + 1);
    return out;
}

/* Load `path`; `shown` is the name to print in diagnostics (the file name as
 * written in the `import` statement, or the path itself for the root file). */
static void load_file(const char *path, const char *shown,
                      const SourceFile *importer, int line, int col) {
    /* Dedupe by canonical path: a file reached twice (diamond) or a cycle
     * back to a file already being loaded is a no-op. */
    char *rp = realpath(path, NULL);
    const char *key = rp ? rp : path;
    if (seen(key)) {
        free(rp);
        return;
    }
    mark_seen(key);
    free(rp);

    struct stat st;
    if (stat(path, &st) != 0) {
        if (importer) {
            fatal_at(importer, line, col, "cannot open imported file '%s'", shown);
        }
        fatal("cannot open '%s': %s", path, strerror(errno));
    }
    if (S_ISDIR(st.st_mode)) {
        if (importer) {
            fatal_at(importer, line, col, "cannot import '%s': it is a directory", shown);
        }
        fatal("cannot open '%s': it is a directory", path);
    }

    const SourceFile *src = read_source(path);
    Token *toks = NULL;
    int ntoks = 0;
    lex(src, &toks, &ntoks);

    if (g_nunits == g_units_cap) {
        g_units_cap = g_units_cap ? g_units_cap * 2 : 8;
        g_units = realloc(g_units, (size_t)g_units_cap * sizeof(Unit));
        if (!g_units) fatal("out of memory");
    }
    g_units[g_nunits].src = src;
    g_units[g_nunits].toks = toks;
    g_units[g_nunits].ntoks = ntoks;
    char *dir = dir_of(path);
    g_units[g_nunits].dir = dir;
    g_nunits++;

    for (int i = 0; i + 1 < ntoks; i++) {
        if (toks[i].kind == TK_IMPORT && toks[i + 1].kind == TK_STRING) {
            load_file(join_path(dir, toks[i + 1].sval), toks[i + 1].sval,
                      src, toks[i + 1].line, toks[i + 1].col);
        }
    }
}

/* Concatenate the declarations of every loaded file into one program; the
 * semantic checks then see a single global namespace, so duplicate names
 * across files are rejected exactly like duplicates inside a file. */
static Program *merge_programs(Program **progs, int n) {
    if (n == 1) return progs[0];

    int nf = 0, nc = 0, ns = 0;
    for (int i = 0; i < n; i++) {
        nf += progs[i]->nfuncs;
        nc += progs[i]->nconsts;
        ns += progs[i]->nstructs;
    }

    Program *m = arena_alloc(sizeof(Program));
    m->nfuncs = nf;
    m->funcs = nf ? arena_alloc((size_t)nf * sizeof(Func *)) : NULL;
    m->nconsts = nc;
    m->consts = nc ? arena_alloc((size_t)nc * sizeof(Const *)) : NULL;
    m->nstructs = ns;
    m->structs = ns ? arena_alloc((size_t)ns * sizeof(StructDecl *)) : NULL;

    int fi = 0, ci = 0, si = 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < progs[i]->nfuncs; j++) m->funcs[fi++] = progs[i]->funcs[j];
        for (int j = 0; j < progs[i]->nconsts; j++) m->consts[ci++] = progs[i]->consts[j];
        for (int j = 0; j < progs[i]->nstructs; j++) m->structs[si++] = progs[i]->structs[j];
    }
    return m;
}

int main(int argc, char **argv) {
    Options opt = parse_args(argc, argv);

    if (!opt.input) {
        fprintf(stderr, "duckyc: error: no input file\n\n");
        usage(stderr);
        return 1;
    }

    if (opt.dump_tokens) {
        /* Dumps the token stream of the file named on the command line. */
        const SourceFile *dsrc = read_source(opt.input);
        Token *dtoks = NULL;
        int dntoks = 0;
        lex(dsrc, &dtoks, &dntoks);
        for (int i = 0; i < dntoks; i++) {
            Token *t = &dtoks[i];
            printf("%4d:%-3d %-12s", t->line, t->col, token_kind_name(t->kind));
            if (t->kind == TK_INT) printf(" %ld", t->ival);
            else if (t->kind == TK_FLOAT) printf(" %g", t->dval);
            else if (t->kind == TK_STRING) printf(" \"%s\"", t->sval);
            else if (t->kind == TK_IDENT) printf(" %s", t->name);
            else if (t->kind != TK_EOF) printf(" %.*s", t->len, t->text);
            putchar('\n');
        }
        return 0;
    }

    load_file(opt.input, opt.input, NULL, 0, 0);

    /* Intern the struct names and the compile-time constants of every file
     * before parsing any of them, so that a struct or a tensor dimension
     * declared in one file can be used in another. */
    for (int i = 0; i < g_nunits; i++) {
        collect_const_ints(g_units[i].src, g_units[i].toks, g_units[i].ntoks);
        intern_struct_declarations(g_units[i].src, g_units[i].toks, g_units[i].ntoks);
    }

    /* Parse each file separately - each parse reports its own file - then
     * merge the declarations and analyze the program as a whole. */
    Program **progs = malloc(sizeof(Program *) * (size_t)g_nunits);
    if (!progs) fatal("out of memory");
    for (int i = 0; i < g_nunits; i++) {
        progs[i] = parse(g_units[i].src, g_units[i].toks, g_units[i].ntoks);
    }
    Program *prog = merge_programs(progs, g_nunits);
    free(progs);

    const SourceFile *src = g_units[0].src;
    analyze(src, prog);

    if (opt.emit_asm) {
        char *derived = NULL;
        const char *out_path = opt.output;
        if (!out_path) {
            derived = derive_path(opt.input, ".s");
            out_path = derived;
        }
        compile_to_assembly(src, prog, out_path);
        free(derived);
        return 0;
    }

    /* Full pipeline: assemble and link. */
    char dir[] = "/tmp/duckyc-XXXXXX";
    if (!mkdtemp(dir)) fatal("cannot create a temporary directory: %s", strerror(errno));

    char asm_path[512];
    char obj_path[512];
    snprintf(asm_path, sizeof(asm_path), "%s/out.s", dir);
    snprintf(obj_path, sizeof(obj_path), "%s/out.o", dir);

    compile_to_assembly(src, prog, asm_path);

    char *derived = NULL;
    const char *out_path = opt.output;
    if (!out_path) {
        derived = derive_path(opt.input, "");
        out_path = derived;
    }

    int status;
    char *as_argv[] = {"as", "--64", "-o", obj_path, asm_path, NULL};
    status = run(as_argv);
    if (status != 0) {
        fprintf(stderr, "duckyc: error: the assembler failed\n");
        goto cleanup;
    }

    /* Programs that declare extern C functions link against the C library
     * (through cc, so the dynamic loader comes with it); pure Ducky programs
     * stay freestanding and link with the raw ld. */
    int has_extern = 0;
    for (int i = 0; i < prog->nfuncs; i++) {
        if (prog->funcs[i]->is_extern) {
            has_extern = 1;
            break;
        }
    }

    if (has_extern) {
        char *cc_argv[] = {"cc", "-nostartfiles", "-o", (char *)out_path,
                           obj_path, "-lm", NULL};
        status = run(cc_argv);
    } else {
        char *ld_argv[] = {"ld", "-o", (char *)out_path, obj_path, NULL};
        status = run(ld_argv);
    }
    if (status != 0) {
        fprintf(stderr, "duckyc: error: the linker failed\n");
        goto cleanup;
    }

cleanup:
    unlink(asm_path);
    unlink(obj_path);
    rmdir(dir);
    free(derived);
    return status;
}
