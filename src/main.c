/* main.c - duckc, the Duck language compiler.
 *
 * Pipeline: source -> tokens -> AST -> type-checked AST -> x86-64 assembly
 * -> object file (GNU as) -> executable (system linker).
 */
#define _POSIX_C_SOURCE 200809L /* mkdtemp, fork, execvp, waitpid */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
            "duckc - the %s language compiler\n"
            "\n"
            "Usage: duckc [options] <file.duck>\n"
            "\n"
            "Options:\n"
            "  -o <path>        write the output to <path>\n"
            "  -S, --emit-asm   stop after writing x86-64 assembly\n"
            "  --dump-tokens    show the token stream and exit\n"
            "  -h, --help       show this help and exit\n"
            "  --version        show version information and exit\n"
            "\n"
            "Without -o, the output name is derived from the input file:\n"
            "  duckc hello.duck   -> ./hello\n"
            "  duckc -S hello.duck -> ./hello.s\n",
            DUCK_LANG_NAME);
}

/* Run a child process and return its exit status. */
static int run(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) fatal("cannot fork: %s", strerror(errno));
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "duckc: error: cannot run '%s': %s\n", argv[0], strerror(errno));
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
            printf("duckc %s (the %s language compiler)\n", DUCKC_VERSION, DUCK_LANG_NAME);
            exit(0);
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "duckc: error: unknown option '%s'\n", a);
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

int main(int argc, char **argv) {
    Options opt = parse_args(argc, argv);

    if (!opt.input) {
        fprintf(stderr, "duckc: error: no input file\n\n");
        usage(stderr);
        return 1;
    }

    const SourceFile *src = read_source(opt.input);

    Token *toks = NULL;
    int ntoks = 0;
    lex(src, &toks, &ntoks);

    if (opt.dump_tokens) {
        for (int i = 0; i < ntoks; i++) {
            Token *t = &toks[i];
            printf("%4d:%-3d %-12s", t->line, t->col, token_kind_name(t->kind));
            if (t->kind == TK_INT) printf(" %ld", t->ival);
            else if (t->kind == TK_STRING) printf(" \"%s\"", t->sval);
            else if (t->kind == TK_IDENT) printf(" %s", t->name);
            else if (t->kind != TK_EOF) printf(" %.*s", t->len, t->text);
            putchar('\n');
        }
        return 0;
    }

    Program *prog = parse(src, toks, ntoks);
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
    char dir[] = "/tmp/duckc-XXXXXX";
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
        fprintf(stderr, "duckc: error: the assembler failed\n");
        goto cleanup;
    }

    char *ld_argv[] = {"ld", "-o", (char *)out_path, obj_path, NULL};
    status = run(ld_argv);
    if (status != 0) {
        fprintf(stderr, "duckc: error: the linker failed\n");
        goto cleanup;
    }

cleanup:
    unlink(asm_path);
    unlink(obj_path);
    rmdir(dir);
    free(derived);
    return status;
}
