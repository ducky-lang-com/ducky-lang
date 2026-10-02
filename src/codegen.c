/* codegen.c - x86-64 code generation (AT&T syntax, System V AMD64 ABI).
 *
 * Code shape
 * ----------
 * Every function uses %rbp as a frame pointer. Parameters and locals live in
 * 8-byte stack slots at negative offsets from %rbp. Expressions are evaluated
 * with a "push machine": intermediate values sit on the hardware stack and
 * the compile-time depth is tracked so that %rsp is always 16-byte aligned
 * right before a `call` (as required by the ABI).
 *
 * The generated program is freestanding: it starts at `_start`, talks to the
 * kernel through `syscall` (write=1, exit=60) and needs no C library.
 */
#include "codegen.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "version.h"

static FILE *O;
static int depth;        /* values currently pushed, known at compile time */
static int label_id;     /* unique id for local labels */
static const char *ret_label; /* epilogue label of the function being emitted */

/* Innermost enclosing loop: where break/continue jump to. */
typedef struct {
    char break_label[32];
    char continue_label[32];
} LoopCtx;
static LoopCtx loops[64];
static int nloops;

/* ---------- string constant pool ---------------------------------------- */

typedef struct {
    const char *text;
    char label[32];
} StrLit;

static StrLit *strs;
static int nstrs;
static int strs_cap;

static const char *str_label(const char *text) {
    for (int i = 0; i < nstrs; i++) {
        if (strcmp(strs[i].text, text) == 0) return strs[i].label;
    }
    if (nstrs == strs_cap) {
        strs_cap = strs_cap ? strs_cap * 2 : 16;
        StrLit *ns = realloc(strs, (size_t)strs_cap * sizeof(StrLit));
        if (!ns) fatal("out of memory");
        strs = ns;
    }
    snprintf(strs[nstrs].label, sizeof(strs[nstrs].label), ".LC%d", nstrs);
    const char *label = strs[nstrs].label;
    strs[nstrs].text = text;
    nstrs++;
    return label;
}

/* ---------- emitters ------------------------------------------------------ */

static void emit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(O, fmt, ap);
    va_end(ap);
}

static void push_rax(void) {
    emit("    push %%rax\n");
    depth++;
}

static void pop_rdi(void) {
    emit("    pop %%rdi\n");
    depth--;
}

/* Emit a call with the stack aligned to 16 bytes, as the ABI requires. */
static void emit_call(const char *target) {
    int pad = (depth & 1) ? 8 : 0;
    if (pad) emit("    sub $8, %%rsp\n");
    emit("    call %s\n", target);
    if (pad) emit("    add $8, %%rsp\n");
}

static int new_label(void) {
    return label_id++;
}

/* ---------- expressions ---------------------------------------------------- */

static void gen_expr(Expr *e);

static const char *arg_regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};

static void gen_logic(Expr *e) {
    int id = new_label();
    int is_and = e->binary.op == BOP_AND;

    gen_expr(e->binary.lhs);
    emit("    cmp $0, %%rax\n");
    if (is_and) emit("    je .Lfalse%d\n", id);
    else emit("    jne .Ltrue%d\n", id);

    gen_expr(e->binary.rhs);
    emit("    cmp $0, %%rax\n");
    if (is_and) emit("    je .Lfalse%d\n", id);
    else emit("    jne .Ltrue%d\n", id);

    if (is_and) {
        emit("    mov $1, %%rax\n    jmp .Lend%d\n", id);
        emit(".Lfalse%d:\n    xor %%rax, %%rax\n", id);
        emit(".Lend%d:\n", id);
    } else {
        emit("    xor %%rax, %%rax\n    jmp .Lend%d\n", id);
        emit(".Ltrue%d:\n    mov $1, %%rax\n", id);
        emit(".Lend%d:\n", id);
    }
}

static void gen_binary(Expr *e) {
    BinaryOp op = e->binary.op;

    if (op == BOP_AND || op == BOP_OR) {
        gen_logic(e);
        return;
    }

    /* String equality compares the contents through the runtime. */
    if ((op == BOP_EQ || op == BOP_NE) && e->binary.lhs->type == TY_STRING) {
        gen_expr(e->binary.lhs);
        push_rax();
        gen_expr(e->binary.rhs);
        pop_rdi(); /* left operand */
        emit("    mov %%rax, %%rsi\n");
        emit_call("duck_streq");
        if (op == BOP_NE) emit("    xor $1, %%eax\n");
        return;
    }

    /* String concatenation allocates through the runtime. */
    if (op == BOP_ADD && e->binary.lhs->type == TY_STRING) {
        gen_expr(e->binary.lhs);
        push_rax();
        gen_expr(e->binary.rhs);
        pop_rdi(); /* left operand */
        emit("    mov %%rax, %%rsi\n");
        emit_call("duck_concat");
        return;
    }

    gen_expr(e->binary.lhs);
    push_rax();
    gen_expr(e->binary.rhs); /* right operand in %rax */
    pop_rdi();                /* left operand in %rdi */

    switch (op) {
    case BOP_ADD:
        emit("    add %%rax, %%rdi\n    mov %%rdi, %%rax\n");
        break;
    case BOP_SUB:
        emit("    sub %%rax, %%rdi\n    mov %%rdi, %%rax\n");
        break;
    case BOP_MUL:
        emit("    imul %%rax, %%rdi\n    mov %%rdi, %%rax\n");
        break;
    case BOP_DIV:
        emit("    mov %%rax, %%rcx\n    mov %%rdi, %%rax\n    cqto\n    idiv %%rcx\n");
        break;
    case BOP_MOD:
        emit("    mov %%rax, %%rcx\n    mov %%rdi, %%rax\n    cqto\n    idiv %%rcx\n");
        emit("    mov %%rdx, %%rax\n");
        break;
    case BOP_BITAND:
        emit("    and %%rax, %%rdi\n    mov %%rdi, %%rax\n");
        break;
    case BOP_BITOR:
        emit("    or %%rax, %%rdi\n    mov %%rdi, %%rax\n");
        break;
    case BOP_XOR:
        emit("    xor %%rax, %%rdi\n    mov %%rdi, %%rax\n");
        break;
    case BOP_SHL:
        /* left operand in %rdi, shift count in %rax -> count to %cl */
        emit("    mov %%rax, %%rcx\n    mov %%rdi, %%rax\n    shl %%cl, %%rax\n");
        break;
    case BOP_SHR:
        emit("    mov %%rax, %%rcx\n    mov %%rdi, %%rax\n    sar %%cl, %%rax\n");
        break;
    case BOP_EQ:
    case BOP_NE:
    case BOP_LT:
    case BOP_LE:
    case BOP_GT:
    case BOP_GE: {
        const char *setcc = op == BOP_EQ   ? "sete"
                            : op == BOP_NE ? "setne"
                            : op == BOP_LT ? "setl"
                            : op == BOP_LE ? "setle"
                            : op == BOP_GT ? "setg"
                                           : "setge";
        emit("    cmp %%rax, %%rdi\n");
        emit("    %s %%al\n", setcc);
        emit("    movzbl %%al, %%eax\n");
        break;
    }
    default:
        fatal("internal error: unhandled binary operator");
    }
}

static void gen_call(Expr *e) {
    if (e->call.builtin != BUILTIN_NONE) {
        switch (e->call.builtin) {
        case BUILTIN_SERVE: {
            /* serve() passes its argument in a register, so the alignment
             * padding can be applied right before the call. */
            Expr *arg = e->call.args[0];
            gen_expr(arg);
            emit("    mov %%rax, %%rdi\n");
            const char *rt = arg->type == TY_INT     ? "duck_serve_int"
                             : arg->type == TY_BOOL  ? "duck_serve_bool"
                                                     : "duck_serve_str";
            emit_call(rt);
            break;
        }
        case BUILTIN_LEN:
            gen_expr(e->call.args[0]);
            emit("    mov %%rax, %%rdi\n");
            emit_call("duck_strlen");
            break;
        case BUILTIN_STR:
            gen_expr(e->call.args[0]);
            emit("    mov %%rax, %%rdi\n");
            emit_call(e->call.args[0]->type == TY_INT ? "duck_str_int"
                                                       : "duck_str_bool");
            break;
        case BUILTIN_INPUT:
            emit_call("duck_input");
            break;
        case BUILTIN_NONE:
            break; /* unreachable (guarded above) */
        }
        return;
    }

    int nargs = e->call.nargs;
    int stack_args = nargs > 6 ? nargs - 6 : 0;

    /* The alignment slot is reserved *before* the arguments are pushed:
     * at the call site %rsp must point at the first stack argument while
     * still being 16-byte aligned. */
    int pad = ((depth + stack_args) & 1) ? 1 : 0;
    if (pad) {
        emit("    sub $8, %%rsp\n");
        depth++;
    }

    /* Arguments are evaluated right to left: after the values that go in
     * registers are popped, the remaining ones already sit at their ABI
     * positions on the stack. */
    for (int i = nargs - 1; i >= 0; i--) {
        gen_expr(e->call.args[i]);
        push_rax();
    }

    int nreg = nargs < 6 ? nargs : 6;
    for (int i = 0; i < nreg; i++) {
        emit("    pop %%%s\n", arg_regs[i]);
        depth--;
    }

    emit("    call %s\n", e->call.fn->name);

    int drop = pad + stack_args;
    if (drop > 0) {
        emit("    add $%d, %%rsp\n", drop * 8);
        depth -= drop;
    }
}

static void gen_expr(Expr *e) {
    switch (e->kind) {
    case EX_INT:
        if (e->ival >= -2147483648L && e->ival <= 2147483647L) {
            emit("    mov $%ld, %%rax\n", e->ival);
        } else {
            emit("    movabs $%ld, %%rax\n", e->ival);
        }
        break;

    case EX_BOOL:
        emit("    mov $%d, %%rax\n", e->bval);
        break;

    case EX_STRING:
        emit("    lea %s(%%rip), %%rax\n", str_label(e->sval));
        break;

    case EX_VAR:
        emit("    mov %d(%%rbp), %%rax\n", e->var.offset);
        break;

    case EX_UNARY:
        gen_expr(e->unary.operand);
        if (e->unary.op == UOP_NEG) {
            emit("    neg %%rax\n");
        } else if (e->unary.op == UOP_BITNOT) {
            emit("    not %%rax\n");
        } else {
            emit("    test %%rax, %%rax\n    sete %%al\n    movzbl %%al, %%eax\n");
        }
        break;

    case EX_BINARY:
        gen_binary(e);
        break;

    case EX_CALL:
        gen_call(e);
        break;
    }
}

/* ---------- statements ------------------------------------------------------ */

static void gen_block(Block *b);

static void gen_stmt(Stmt *s) {
    switch (s->kind) {
    case ST_LET:
        gen_expr(s->let.init);
        emit("    mov %%rax, %d(%%rbp)\n", s->let.offset);
        break;

    case ST_ASSIGN:
        gen_expr(s->assign.value);
        emit("    mov %%rax, %d(%%rbp)\n", s->assign.offset);
        break;

    case ST_EXPR:
        gen_expr(s->expr);
        break;

    case ST_BLOCK:
        gen_block(s->block);
        break;

    case ST_IF: {
        int id = new_label();
        gen_expr(s->ifs.cond);
        emit("    cmp $0, %%rax\n    je .Lelse%d\n", id);
        gen_block(s->ifs.then_block);
        if (s->ifs.else_block) {
            emit("    jmp .Lend%d\n", id);
            emit(".Lelse%d:\n", id);
            gen_block(s->ifs.else_block);
            emit(".Lend%d:\n", id);
        } else {
            emit(".Lelse%d:\n", id);
        }
        break;
    }

    case ST_WHILE: {
        int id = new_label();
        LoopCtx ctx;
        snprintf(ctx.break_label, sizeof(ctx.break_label), ".Lend%d", id);
        snprintf(ctx.continue_label, sizeof(ctx.continue_label), ".Lcond%d", id);
        if (nloops >= (int)(sizeof(loops) / sizeof(loops[0]))) {
            fatal("internal error: loops nested too deeply");
        }
        loops[nloops++] = ctx;

        emit(".Lcond%d:\n", id);
        gen_expr(s->whiles.cond);
        emit("    cmp $0, %%rax\n    je .Lend%d\n", id);
        gen_block(s->whiles.body);
        emit("    jmp .Lcond%d\n", id);
        emit(".Lend%d:\n", id);
        nloops--;
        break;
    }

    case ST_FOR: {
        int id = new_label();
        LoopCtx ctx;
        snprintf(ctx.break_label, sizeof(ctx.break_label), ".Lend%d", id);
        snprintf(ctx.continue_label, sizeof(ctx.continue_label), ".Lstep%d", id);
        if (nloops >= (int)(sizeof(loops) / sizeof(loops[0]))) {
            fatal("internal error: loops nested too deeply");
        }
        loops[nloops++] = ctx;

        /* Both bounds are evaluated exactly once: the start lands in the
         * loop variable, the end in a hidden slot. */
        gen_expr(s->fors.start);
        emit("    mov %%rax, %d(%%rbp)\n", s->fors.var_offset);
        gen_expr(s->fors.end);
        emit("    mov %%rax, %d(%%rbp)\n", s->fors.end_offset);

        emit(".Lcond%d:\n", id);
        emit("    mov %d(%%rbp), %%rax\n", s->fors.var_offset);
        emit("    mov %d(%%rbp), %%rdi\n", s->fors.end_offset);
        emit("    cmp %%rdi, %%rax\n");
        emit("    jge .Lend%d\n", id);
        gen_block(s->fors.body);

        emit(".Lstep%d:\n", id);
        emit("    mov %d(%%rbp), %%rax\n", s->fors.var_offset);
        emit("    add $1, %%rax\n");
        emit("    mov %%rax, %d(%%rbp)\n", s->fors.var_offset);
        emit("    jmp .Lcond%d\n", id);
        emit(".Lend%d:\n", id);
        nloops--;
        break;
    }

    case ST_BREAK:
        if (nloops == 0) fatal("internal error: break outside of a loop");
        emit("    jmp %s\n", loops[nloops - 1].break_label);
        break;

    case ST_CONTINUE:
        if (nloops == 0) fatal("internal error: continue outside of a loop");
        emit("    jmp %s\n", loops[nloops - 1].continue_label);
        break;

    case ST_RETURN:
        if (s->value) gen_expr(s->value);
        emit("    jmp %s\n", ret_label);
        break;
    }
}

static void gen_block(Block *b) {
    for (int i = 0; i < b->nstmts; i++) gen_stmt(b->stmts[i]);
}

static void gen_func(Func *f, int index) {
    char ret[64];
    snprintf(ret, sizeof(ret), ".Lret%d", index);
    ret_label = ret;

    emit("\n    .globl %s\n", f->name);
    emit("%s:\n", f->name);
    emit("    push %%rbp\n");
    emit("    mov %%rsp, %%rbp\n");
    if (f->frame_size > 0) emit("    sub $%d, %%rsp\n", f->frame_size);

    for (int i = 0; i < f->nparams && i < 6; i++) {
        emit("    mov %%%s, %d(%%rbp)\n", arg_regs[i], -8 * (i + 1));
    }
    /* Parameters beyond the sixth arrive on the stack; move them into slots. */
    for (int i = 6; i < f->nparams; i++) {
        int incoming = 16 + 8 * (i - 6); /* return address + saved %rbp */
        emit("    mov %d(%%rbp), %%rax\n", incoming);
        emit("    mov %%rax, %d(%%rbp)\n", -8 * (i + 1));
    }

    depth = 0;
    nloops = 0;
    gen_block(f->body);

    emit("%s:\n", ret);
    emit("    mov %%rbp, %%rsp\n");
    emit("    pop %%rbp\n");
    emit("    ret\n");
}

/* ---------- runtime support -------------------------------------------------- */

static void emit_runtime(void) {
    emit("    .globl _start\n");
    emit("_start:\n");
    emit("    xor %%ebp, %%ebp\n");
    emit("    and $-16, %%rsp\n");
    emit("    call main\n");
    emit("    mov %%rax, %%rdi\n");
    emit("    mov $60, %%eax\n");
    emit("    syscall\n");

    /* serve(value: int) -> writes the decimal value and a newline */
    emit("\nduck_serve_int:\n");
    emit("    push %%rbp\n");
    emit("    mov %%rsp, %%rbp\n");
    emit("    sub $32, %%rsp\n");
    emit("    mov %%rdi, %%rax\n");
    emit("    mov %%rbp, %%r8\n");
    emit("    mov $10, %%r10\n");
    emit("    test %%rax, %%rax\n");
    emit("    jns .Lsi_pos\n");
    emit("    neg %%rax\n");
    emit(".Lsi_pos:\n");
    emit(".Lsi_loop:\n");
    emit("    xor %%edx, %%edx\n");
    emit("    div %%r10\n");
    emit("    add $48, %%dl\n");
    emit("    dec %%r8\n");
    emit("    mov %%dl, (%%r8)\n");
    emit("    test %%rax, %%rax\n");
    emit("    jnz .Lsi_loop\n");
    emit("    test %%rdi, %%rdi\n");
    emit("    jns .Lsi_write\n");
    emit("    dec %%r8\n");
    emit("    movb $45, (%%r8)\n");
    emit(".Lsi_write:\n");
    emit("    mov %%rbp, %%rdx\n");
    emit("    sub %%r8, %%rdx\n");
    emit("    mov %%r8, %%rsi\n");
    emit("    mov $1, %%eax\n");
    emit("    mov $1, %%edi\n");
    emit("    syscall\n");
    emit("    lea .Lnl(%%rip), %%rsi\n");
    emit("    mov $1, %%edx\n");
    emit("    mov $1, %%eax\n");
    emit("    mov $1, %%edi\n");
    emit("    syscall\n");
    emit("    mov %%rbp, %%rsp\n");
    emit("    pop %%rbp\n");
    emit("    ret\n");

    /* serve(value: bool) -> writes true/false and a newline */
    emit("\nduck_serve_bool:\n");
    emit("    push %%rbp\n");
    emit("    mov %%rsp, %%rbp\n");
    emit("    test %%rdi, %%rdi\n");
    emit("    jz .Lsb_false\n");
    emit("    lea .Ltrue(%%rip), %%rsi\n");
    emit("    mov $5, %%edx\n");
    emit("    jmp .Lsb_write\n");
    emit(".Lsb_false:\n");
    emit("    lea .Lfalse(%%rip), %%rsi\n");
    emit("    mov $6, %%edx\n");
    emit(".Lsb_write:\n");
    emit("    mov $1, %%eax\n");
    emit("    mov $1, %%edi\n");
    emit("    syscall\n");
    emit("    pop %%rbp\n");
    emit("    ret\n");

    /* serve(value: string) -> writes the string and a newline */
    emit("\nduck_serve_str:\n");
    emit("    push %%rbp\n");
    emit("    mov %%rsp, %%rbp\n");
    emit("    mov %%rdi, %%rsi\n");
    emit("    xor %%edx, %%edx\n");
    emit(".Lss_len:\n");
    emit("    cmpb $0, (%%rsi, %%rdx)\n");
    emit("    je .Lss_write\n");
    emit("    inc %%rdx\n");
    emit("    jmp .Lss_len\n");
    emit(".Lss_write:\n");
    emit("    mov $1, %%eax\n");
    emit("    mov $1, %%edi\n");
    emit("    syscall\n");
    emit("    lea .Lnl(%%rip), %%rsi\n");
    emit("    mov $1, %%edx\n");
    emit("    mov $1, %%eax\n");
    emit("    mov $1, %%edi\n");
    emit("    syscall\n");
    emit("    mov %%rbp, %%rsp\n");
    emit("    pop %%rbp\n");
    emit("    ret\n");

    /* streq(a: string, b: string) -> 1 when the contents are equal */
    emit("\nduck_streq:\n");
    emit(".Lse_loop:\n");
    emit("    movb (%%rdi), %%al\n");
    emit("    movb (%%rsi), %%cl\n");
    emit("    cmpb %%cl, %%al\n");
    emit("    jne .Lse_no\n");
    emit("    testb %%al, %%al\n");
    emit("    je .Lse_yes\n");
    emit("    inc %%rdi\n");
    emit("    inc %%rsi\n");
    emit("    jmp .Lse_loop\n");
    emit(".Lse_yes:\n");
    emit("    mov $1, %%eax\n");
    emit("    ret\n");
    emit(".Lse_no:\n");
    emit("    xor %%eax, %%eax\n");
    emit("    ret\n");

    /* len(s) -> byte length of a string */
    emit("\nduck_strlen:\n");
    emit("    xor %%eax, %%eax\n");
    emit(".Lsl_loop:\n");
    emit("    cmpb $0, (%%rdi, %%rax)\n");
    emit("    je .Lsl_done\n");
    emit("    inc %%rax\n");
    emit("    jmp .Lsl_loop\n");
    emit(".Lsl_done:\n");
    emit("    ret\n");

    /* duck_alloc(size) -> fresh zero page memory from the program break.
     * A bump allocator: memory is never freed (there is no garbage
     * collector); on exhaustion the program stops with a clear message. */
    emit("\nduck_alloc:\n");
    emit("    push %%rbx\n");
    emit("    push %%r12\n");
    emit("    mov %%rdi, %%rbx\n");           /* rbx = size */
    emit("    mov duck_brk(%%rip), %%rdi\n");
    emit("    test %%rdi, %%rdi\n");
    emit("    jne .La_base\n");
    emit("    xor %%edi, %%edi\n");
    emit("    mov $12, %%eax\n");             /* sys_brk(0) -> current break */
    emit("    syscall\n");
    emit("    mov %%rax, %%rdi\n");
    emit(".La_base:\n");
    emit("    mov %%rdi, %%r12\n");           /* r12 = block handed out */
    emit("    lea 15(%%rdi, %%rbx), %%rdi\n");
    emit("    and $-16, %%rdi\n");            /* round the new break up */
    emit("    mov $12, %%eax\n");             /* sys_brk(new) */
    emit("    syscall\n");
    emit("    cmp %%rdi, %%rax\n");
    emit("    jb .La_oom\n");                 /* kernel refused: out of memory */
    emit("    mov %%rdi, duck_brk(%%rip)\n"); /* commit the bump pointer */
    emit("    mov %%r12, %%rax\n");
    emit("    pop %%r12\n");
    emit("    pop %%rbx\n");
    emit("    ret\n");
    emit(".La_oom:\n");
    emit("    lea .Loommsg(%%rip), %%rsi\n");
    emit("    mov $20, %%edx\n");
    emit("    mov $2, %%edi\n");
    emit("    mov $1, %%eax\n");              /* sys_write(2, ...) */
    emit("    syscall\n");
    emit("    mov $127, %%edi\n");
    emit("    mov $60, %%eax\n");             /* sys_exit(127) */
    emit("    syscall\n");

    /* duck_concat(left, right) -> newly allocated left+right */
    emit("\nduck_concat:\n");
    emit("    push %%rbx\n");
    emit("    push %%r12\n");
    emit("    push %%r13\n");
    emit("    mov %%rdi, %%rbx\n");           /* left */
    emit("    mov %%rsi, %%r12\n");           /* right */
    emit("    call duck_strlen\n");           /* rdi is still left */
    emit("    mov %%rax, %%r13\n");
    emit("    mov %%r12, %%rdi\n");
    emit("    call duck_strlen\n");
    emit("    lea 1(%%r13, %%rax), %%rdi\n"); /* len(left)+len(right)+1 */
    emit("    call duck_alloc\n");
    emit("    mov %%rax, %%r8\n");            /* dst */
    emit("    cld\n");
    emit("    mov %%rbx, %%rsi\n");           /* copy left ... */
    emit("    mov %%r8, %%rdi\n");
    emit("    mov %%r13, %%rcx\n");
    emit("    rep movsb\n");
    emit("    mov %%r12, %%rdi\n");
    emit("    call duck_strlen\n");           /* ... then right + its NUL */
    emit("    mov %%rax, %%rcx\n");
    emit("    inc %%rcx\n");
    emit("    mov %%r12, %%rsi\n");
    emit("    mov %%r8, %%rdi\n");
    emit("    add %%r13, %%rdi\n");
    emit("    rep movsb\n");
    emit("    mov %%r8, %%rax\n");
    emit("    pop %%r13\n");
    emit("    pop %%r12\n");
    emit("    pop %%rbx\n");
    emit("    ret\n");

    /* str(value: int) -> decimal representation on the heap */
    emit("\nduck_str_int:\n");
    emit("    push %%rbp\n");
    emit("    push %%rbx\n");
    emit("    mov %%rsp, %%rbp\n");
    emit("    sub $40, %%rsp\n");             /* conversion buffer */
    emit("    mov %%rdi, %%rax\n");
    emit("    mov %%rbp, %%r8\n");            /* digits written downwards */
    emit("    mov $10, %%r9\n");
    emit("    xor %%r11, %%r11\n");           /* sign flag */
    emit("    test %%rax, %%rax\n");
    emit("    jns .Lti_pos\n");
    emit("    mov $1, %%r11\n");
    emit("    neg %%rax\n");
    emit(".Lti_pos:\n");
    emit(".Lti_loop:\n");
    emit("    xor %%edx, %%edx\n");
    emit("    div %%r9\n");
    emit("    add $48, %%dl\n");
    emit("    dec %%r8\n");
    emit("    mov %%dl, (%%r8)\n");
    emit("    test %%rax, %%rax\n");
    emit("    jnz .Lti_loop\n");
    emit("    test %%r11, %%r11\n");
    emit("    jz .Lti_copy\n");
    emit("    dec %%r8\n");
    emit("    movb $45, (%%r8)\n");           /* '-' */
    emit(".Lti_copy:\n");
    emit("    mov %%rbp, %%rbx\n");
    emit("    sub %%r8, %%rbx\n");            /* rbx = length */
    emit("    lea 1(%%rbx), %%rdi\n");
    emit("    push %%r8\n");                  /* src ... */
    emit("    push %%r8\n");                  /* ... twice: keep alignment */
    emit("    call duck_alloc\n");
    emit("    pop %%rsi\n");                  /* src */
    emit("    pop %%rdx\n");                  /* (discard) */
    emit("    mov %%rax, %%rdi\n");
    emit("    mov %%rbx, %%rcx\n");
    emit("    push %%rax\n");                 /* dst */
    emit("    cld\n");
    emit("    rep movsb\n");
    emit("    pop %%rax\n");
    emit("    movb $0, (%%rax, %%rbx)\n");
    emit("    mov %%rbp, %%rsp\n");
    emit("    pop %%rbx\n");
    emit("    pop %%rbp\n");
    emit("    ret\n");

    /* str(value: bool) -> "true" / "false" (never mutated: static) */
    emit("\nduck_str_bool:\n");
    emit("    test %%rdi, %%rdi\n");
    emit("    jz .Ltb_false\n");
    emit("    lea .Lstr_true(%%rip), %%rax\n");
    emit("    ret\n");
    emit(".Ltb_false:\n");
    emit("    lea .Lstr_false(%%rip), %%rax\n");
    emit("    ret\n");

    /* input_line() -> one line from stdin without the newline. It reads one
     * byte at a time so the next call starts exactly at the next line. */
    emit("\nduck_input:\n");
    emit("    push %%rbp\n");
    emit("    push %%r12\n");
    emit("    mov %%rsp, %%rbp\n");
    emit("    sub $4104, %%rsp\n");           /* 4096-byte buffer + slack */
    emit("    mov %%rsp, %%r12\n");           /* cursor */
    emit(".Lin_loop:\n");
    emit("    lea 4095(%%rsp), %%rax\n");     /* buffer limit */
    emit("    cmp %%rax, %%r12\n");
    emit("    jae .Lin_done\n");              /* full: return the partial line */
    emit("    xor %%edi, %%edi\n");           /* fd 0 */
    emit("    mov %%r12, %%rsi\n");           /* &buf[len] */
    emit("    mov $1, %%edx\n");              /* one byte */
    emit("    xor %%eax, %%eax\n");           /* sys_read */
    emit("    syscall\n");
    emit("    test %%rax, %%rax\n");
    emit("    jle .Lin_done\n");              /* EOF (or error) */
    emit("    movb (%%r12), %%al\n");
    emit("    inc %%r12\n");
    emit("    cmp $10, %%al\n");
    emit("    jne .Lin_loop\n");              /* newline: drop it below */
    emit("    dec %%r12\n");                  /* the newline is not copied */
    emit(".Lin_done:\n");
    emit("    movb $0, (%%r12)\n");           /* NUL-terminate */
    emit("    mov %%r12, %%rdi\n");
    emit("    sub %%rsp, %%rdi\n");           /* len */
    emit("    inc %%rdi\n");                  /* len + 1 for the NUL */
    emit("    call duck_alloc\n");
    emit("    mov %%rax, %%rdi\n");           /* dst */
    emit("    mov %%rsp, %%rsi\n");           /* src = buffer */
    emit("    mov %%r12, %%rcx\n");
    emit("    sub %%rsp, %%rcx\n");
    emit("    inc %%rcx\n");                  /* copy the NUL too */
    emit("    cld\n");
    emit("    rep movsb\n");
    emit("    mov %%rbp, %%rsp\n");
    emit("    pop %%r12\n");
    emit("    pop %%rbp\n");
    emit("    ret\n");
}

/* ---------- assembly output --------------------------------------------------- */

static void emit_string_bytes(const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') emit("\\\"");
        else if (c == '\\') emit("\\\\");
        else if (c == '\n') emit("\\n");
        else if (c == '\t') emit("\\t");
        else if (c == '\r') emit("\\r");
        else if (c >= 32 && c < 127) emit("%c", c);
        else emit("\\%03o", c);
    }
}

void generate(const SourceFile *src, Program *prog, FILE *out) {
    O = out;
    depth = 0;
    label_id = 0;
    nstrs = 0;

    emit("/* Generated by duckc %s from %s - do not edit. */\n", DUCKC_VERSION, src->path);
    emit("    .section .note.GNU-stack,\"\",@progbits\n");
    emit("    .text\n");

    emit_runtime();

    for (int i = 0; i < prog->nfuncs; i++) gen_func(prog->funcs[i], i);

    emit("\n    .section .rodata\n");
    emit(".Lnl:\n    .ascii \"\\n\"\n");
    emit(".Ltrue:\n    .ascii \"true\\n\"\n");
    emit(".Lfalse:\n    .ascii \"false\\n\"\n");
    emit(".Lstr_true:\n    .string \"true\"\n");
    emit(".Lstr_false:\n    .string \"false\"\n");
    emit(".Lempty:\n    .string \"\"\n");
    emit(".Loommsg:\n    .ascii \"duck: out of memory\\n\"\n");
    for (int i = 0; i < nstrs; i++) {
        emit("%s:\n    .string \"", strs[i].label);
        emit_string_bytes(strs[i].text);
        emit("\"\n");
    }

    emit("\n    .section .bss\n");
    emit("    .align 8\n");
    emit("duck_brk:\n    .zero 8\n");
}
