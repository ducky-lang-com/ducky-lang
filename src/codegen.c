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
static int g_libc;       /* the program declares extern C functions */

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

    /* Floats travel as their 64-bit representation in the integer
     * registers and only enter the XMM registers for the operation
     * itself, so the push machine works unchanged. */
    if (e->binary.lhs->type == TY_FLOAT && e->binary.rhs->type == TY_FLOAT) {
        gen_expr(e->binary.lhs);
        push_rax();
        gen_expr(e->binary.rhs);
        pop_rdi(); /* left operand bits in %rdi */
        emit("    movq %%rdi, %%xmm0\n");
        emit("    movq %%rax, %%xmm1\n");
        switch (op) {
        case BOP_ADD:
            emit("    addsd %%xmm1, %%xmm0\n    movq %%xmm0, %%rax\n");
            return;
        case BOP_SUB:
            emit("    subsd %%xmm1, %%xmm0\n    movq %%xmm0, %%rax\n");
            return;
        case BOP_MUL:
            emit("    mulsd %%xmm1, %%xmm0\n    movq %%xmm0, %%rax\n");
            return;
        case BOP_DIV:
            emit("    divsd %%xmm1, %%xmm0\n    movq %%xmm0, %%rax\n");
            return;
        case BOP_EQ:
        case BOP_NE:
        case BOP_LT:
        case BOP_LE:
        case BOP_GT:
        case BOP_GE:
            /* ucomisd sets CF on "less than OR unordered", so the ordered
             * cases also test PF; NaN compares false everywhere except
             * '!=' (IEEE-754 semantics). */
            emit("    ucomisd %%xmm1, %%xmm0\n");
            switch (op) {
            case BOP_EQ:
                emit("    sete %%al\n    setnp %%cl\n    and %%cl, %%al\n");
                break;
            case BOP_NE:
                emit("    setne %%al\n    setp %%cl\n    or %%cl, %%al\n");
                break;
            case BOP_LT:
                emit("    setb %%al\n    setnp %%cl\n    and %%cl, %%al\n");
                break;
            case BOP_LE:
                emit("    setbe %%al\n    setnp %%cl\n    and %%cl, %%al\n");
                break;
            case BOP_GT:
                emit("    seta %%al\n");
                break;
            default:
                emit("    setae %%al\n");
                break;
            }
            emit("    movzbl %%al, %%eax\n");
            return;
        default:
            fatal("internal error: unhandled float operator");
        }
    }

    /* String equality compares the contents through the runtime. */
    if ((op == BOP_EQ || op == BOP_NE) && e->binary.lhs->type == TY_STRING) {
        gen_expr(e->binary.lhs);
        push_rax();
        gen_expr(e->binary.rhs);
        pop_rdi(); /* left operand */
        emit("    mov %%rax, %%rsi\n");
        emit_call("ducky_streq");
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
        emit_call("ducky_concat");
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

/* Calls to C functions: the System V ABI assigns integer-class arguments to
 * rdi..r9 and float arguments to xmm0..xmm7 independently of their
 * positions; an argument whose class has run out of registers goes on the
 * stack, in argument order. Ducky keeps every value in %rax until the call,
 * so all arguments are pushed first (argument i sits at 8*i(%rsp)), then
 * placed where C expects them. %al must hold the number of vector registers
 * in use, which is what variadic functions like printf read. */
static void gen_call_extern(Expr *e) {
    Func *fn = e->call.fn;
    int nargs = e->call.nargs;

    /* The alignment pad is reserved before the pushes: at the call site the
     * whole argument block is still on the stack and %rsp must be aligned. */
    int pad = ((depth + nargs) & 1) ? 1 : 0;
    if (pad) {
        emit("    sub $8, %%rsp\n");
        depth++;
    }

    /* Arguments are evaluated right to left, as in ordinary calls. */
    for (int i = nargs - 1; i >= 0; i--) {
        gen_expr(e->call.args[i]);
        push_rax();
    }

    /* 1. Register arguments. Everything is already evaluated, so no nested
     *    call can clobber the registers while we place them. */
    int gi = 0, xi = 0;
    for (int i = 0; i < nargs; i++) {
        if (e->call.args[i]->type == TY_FLOAT) {
            if (xi < 8) {
                emit("    movq %d(%%rsp), %%xmm%d\n", 8 * i, xi);
                xi++;
            }
        } else if (gi < 6) {
            emit("    mov %d(%%rsp), %%%s\n", 8 * i, arg_regs[gi]);
            gi++;
        }
    }

    /* 2. Stack arguments: slide each one down to its compacted slot. Targets
     *    are always below their sources, so no slot is read twice. */
    int sidx = 0;
    gi = 0;
    xi = 0;
    for (int i = 0; i < nargs; i++) {
        int on_stack;
        if (e->call.args[i]->type == TY_FLOAT) {
            on_stack = (xi >= 8);
            if (!on_stack) xi++;
        } else {
            on_stack = (gi >= 6);
            if (!on_stack) gi++;
        }
        if (on_stack) {
            if (sidx != i) {
                emit("    mov %d(%%rsp), %%rax\n", 8 * i);
                emit("    mov %%rax, %d(%%rsp)\n", 8 * sidx);
            }
            sidx++;
        }
    }

    emit("    mov $%d, %%eax\n", xi); /* %al: vector registers in use */
    emit("    call %s\n", fn->name);
    if (fn->ret == TY_FLOAT) emit("    movq %%xmm0, %%rax\n");

    int drop = pad + nargs;
    if (drop > 0) {
        emit("    add $%d, %%rsp\n", drop * 8);
        depth -= drop;
    }
}

static void gen_call(Expr *e) {
    if (e->call.sdef) {
        /* Struct constructor: one heap block of 8-byte field slots. The
         * block waits in a hidden stack slot while the fields are
         * evaluated, so constructors nest safely (array-literal shape). */
        StructDecl *sd = e->call.sdef;
        int size = 8 * sd->nfields;
        if (size == 0) size = 8; /* an empty struct still owns a block */
        emit("    mov $%d, %%rdi\n", size);
        emit_call("ducky_alloc");
        emit("    mov %%rax, %d(%%rbp)\n", e->call.slot);
        for (int i = 0; i < sd->nfields; i++) {
            gen_expr(e->call.args[i]);
            emit("    mov %d(%%rbp), %%rdi\n", e->call.slot);
            emit("    mov %%rax, %d(%%rdi)\n", 8 * i);
        }
        emit("    mov %d(%%rbp), %%rax\n", e->call.slot);
        return;
    }
    if (e->call.builtin != BUILTIN_NONE) {
        switch (e->call.builtin) {
        case BUILTIN_SERVE: {
            /* serve() passes its argument in a register, so the alignment
             * padding can be applied right before the call. */
            Expr *arg = e->call.args[0];
            gen_expr(arg);
            emit("    mov %%rax, %%rdi\n");
            const char *rt = arg->type == TY_INT     ? "ducky_serve_int"
                             : arg->type == TY_BOOL  ? "ducky_serve_bool"
                             : arg->type == TY_FLOAT ? "ducky_serve_float"
                                                     : "ducky_serve_str";
            emit_call(rt);
            break;
        }
        case BUILTIN_LEN:
            gen_expr(e->call.args[0]);
            if (type_is_array(e->call.args[0]->type)) {
                emit("    mov (%%rax), %%rax\n"); /* count lives in the header */
            } else {
                emit("    mov %%rax, %%rdi\n");
                emit_call("ducky_strlen");
            }
            break;
        case BUILTIN_STR: {
            gen_expr(e->call.args[0]);
            emit("    mov %%rax, %%rdi\n");
            Type t = e->call.args[0]->type;
            emit_call(t == TY_INT   ? "ducky_str_int"
                      : t == TY_FLOAT ? "ducky_str_float"
                                      : "ducky_str_bool");
            break;
        }
        case BUILTIN_INPUT:
            emit_call("ducky_input");
            break;
        case BUILTIN_INT:
            gen_expr(e->call.args[0]);
            emit("    movq %%rax, %%xmm0\n");
            emit("    cvttsd2si %%xmm0, %%rax\n");
            break;
        case BUILTIN_FLOAT:
            gen_expr(e->call.args[0]);
            emit("    cvtsi2sd %%rax, %%xmm0\n");
            emit("    movq %%xmm0, %%rax\n");
            break;
        case BUILTIN_PUSH:
            gen_expr(e->call.args[0]);
            push_rax();
            gen_expr(e->call.args[1]);
            emit("    mov %%rax, %%rsi\n");
            pop_rdi(); /* array in %rdi, value in %rsi */
            emit_call("ducky_push");
            break;
        case BUILTIN_SCAN_INT:
            gen_expr(e->call.args[0]);
            emit("    mov %%rax, %%rdi\n");
            emit_call("ducky_scan_int");
            break;
        case BUILTIN_SCAN_FLOAT:
            gen_expr(e->call.args[0]);
            emit("    mov %%rax, %%rdi\n");
            emit_call("ducky_scan_float");
            break;
        case BUILTIN_SCAN_INT_LINE:
            emit_call("ducky_input");
            emit("    mov %%rax, %%rdi\n");
            emit_call("ducky_scan_int");
            break;
        case BUILTIN_SCAN_FLOAT_LINE:
            emit_call("ducky_input");
            emit("    mov %%rax, %%rdi\n");
            emit_call("ducky_scan_float");
            break;
        case BUILTIN_NONE:
            break; /* unreachable (guarded above) */
        }
        return;
    }

    if (e->call.fn->is_extern) {
        gen_call_extern(e);
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

    case EX_FLOAT: {
        unsigned long long bits;
        double d = e->dval;
        memcpy(&bits, &d, sizeof(bits));
        emit("    movabs $0x%llx, %%rax\n", bits);
        break;
    }

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
            if (e->unary.operand->type == TY_FLOAT) {
                emit("    movabs $0x8000000000000000, %%rcx\n");
                emit("    xor %%rcx, %%rax\n");
            } else {
                emit("    neg %%rax\n");
            }
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

    case EX_ARRAY: {
        /* Layout: [count:int64][elem0]...[elemN-1], one heap block.
         * The block pointer waits in a hidden stack slot while the
         * elements are evaluated, so array literals nest safely. */
        int size = 8 + 8 * e->array.nelems;
        emit("    mov $%d, %%rdi\n", size);
        emit_call("ducky_alloc");
        emit("    mov %%rax, %d(%%rbp)\n", e->array.slot);
        emit("    movq $%d, (%%rax)\n", e->array.nelems);
        for (int i = 0; i < e->array.nelems; i++) {
            gen_expr(e->array.elems[i]);
            emit("    mov %d(%%rbp), %%rdi\n", e->array.slot);
            emit("    mov %%rax, %d(%%rdi)\n", 8 + 8 * i);
        }
        emit("    mov %d(%%rbp), %%rax\n", e->array.slot);
        break;
    }

    case EX_INDEX: {
        Type ot = e->index.obj->type;
        gen_expr(e->index.obj);
        push_rax();
        gen_expr(e->index.idx);
        pop_rdi(); /* object in %rdi, index in %rax */
        if (ot == TY_STRING) {
            emit("    mov %%rax, %%rsi\n");
            emit_call("ducky_str_at");
        } else {
            emit("    mov (%%rdi), %%rcx\n"); /* element count */
            emit("    cmp %%rcx, %%rax\n");
            emit("    jae ducky_oob\n"); /* unsigned: also catches negatives */
            emit("    mov 8(%%rdi, %%rax, 8), %%rax\n");
        }
        break;
    }

    case EX_FIELD:
        /* Struct values are block pointers: the field is the 8-byte slot
         * at a fixed offset, chosen by semantic analysis. */
        gen_expr(e->field.obj);
        emit("    mov %d(%%rax), %%rax\n", e->field.offset);
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
        if (s->assign.target) {
            Expr *tg = s->assign.target;
            if (tg->kind == EX_FIELD) {
                /* `p.field = v;` - evaluate the struct, then the value. */
                gen_expr(tg->field.obj);
                push_rax();
                gen_expr(s->assign.value);
                pop_rdi(); /* block in %rdi, value in %rax */
                emit("    mov %%rax, %d(%%rdi)\n", tg->field.offset);
                break;
            }
            /* `xs[i] = v;` - bounds-check, form &xs[i], then store. */
            gen_expr(tg->index.obj);
            push_rax();
            gen_expr(tg->index.idx);
            pop_rdi(); /* object in %rdi, index in %rax */
            emit("    mov (%%rdi), %%rcx\n");
            emit("    cmp %%rcx, %%rax\n");
            emit("    jae ducky_oob\n");
            emit("    lea 8(%%rdi, %%rax, 8), %%rax\n");
            push_rax(); /* &element */
            gen_expr(s->assign.value);
            pop_rdi();
            emit("    mov %%rax, (%%rdi)\n");
            break;
        }
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
    if (f->is_extern) return; /* declared elsewhere: there is no body to emit */

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
    if (g_libc) {
        /* Linked against libc: there is no crt runtime here, so flush every
         * stdio stream ourselves before the raw exit syscall. */
        emit("    mov %%rax, %%r12\n");     /* exit code while fflush runs */
        emit("    xor %%edi, %%edi\n");     /* fflush(NULL) */
        emit("    call fflush\n");
        emit("    mov %%r12, %%rdi\n");
    } else {
        emit("    mov %%rax, %%rdi\n");
    }
    emit("    mov $60, %%eax\n");
    emit("    syscall\n");

    /* serve(value: int) -> writes the decimal value and a newline */
    emit("\nducky_serve_int:\n");
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
    emit("\nducky_serve_bool:\n");
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
    emit("\nducky_serve_str:\n");
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
    emit("\nducky_streq:\n");
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
    emit("\nducky_strlen:\n");
    emit("    xor %%eax, %%eax\n");
    emit(".Lsl_loop:\n");
    emit("    cmpb $0, (%%rdi, %%rax)\n");
    emit("    je .Lsl_done\n");
    emit("    inc %%rax\n");
    emit("    jmp .Lsl_loop\n");
    emit(".Lsl_done:\n");
    emit("    ret\n");

    /* ducky_alloc(size) -> fresh zero page memory from the program break.
     * A bump allocator: memory is never freed (there is no garbage
     * collector); on exhaustion the program stops with a clear message. */
    emit("\nducky_alloc:\n");
    emit("    push %%rbx\n");
    emit("    push %%r12\n");
    emit("    mov %%rdi, %%rbx\n");           /* rbx = size */
    emit("    mov ducky_brk(%%rip), %%rdi\n");
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
    emit("    mov %%rdi, ducky_brk(%%rip)\n"); /* commit the bump pointer */
    emit("    mov %%r12, %%rax\n");
    emit("    pop %%r12\n");
    emit("    pop %%rbx\n");
    emit("    ret\n");
    emit(".La_oom:\n");
    emit("    lea .Loommsg(%%rip), %%rsi\n");
    emit("    mov $21, %%edx\n");
    emit("    mov $2, %%edi\n");
    emit("    mov $1, %%eax\n");              /* sys_write(2, ...) */
    emit("    syscall\n");
    emit("    mov $127, %%edi\n");
    emit("    mov $60, %%eax\n");             /* sys_exit(127) */
    emit("    syscall\n");

    /* ducky_concat(left, right) -> newly allocated left+right */
    emit("\nducky_concat:\n");
    emit("    push %%rbx\n");
    emit("    push %%r12\n");
    emit("    push %%r13\n");
    emit("    mov %%rdi, %%rbx\n");           /* left */
    emit("    mov %%rsi, %%r12\n");           /* right */
    emit("    call ducky_strlen\n");           /* rdi is still left */
    emit("    mov %%rax, %%r13\n");
    emit("    mov %%r12, %%rdi\n");
    emit("    call ducky_strlen\n");
    emit("    lea 1(%%r13, %%rax), %%rdi\n"); /* len(left)+len(right)+1 */
    emit("    call ducky_alloc\n");
    emit("    mov %%rax, %%r8\n");            /* dst */
    emit("    cld\n");
    emit("    mov %%rbx, %%rsi\n");           /* copy left ... */
    emit("    mov %%r8, %%rdi\n");
    emit("    mov %%r13, %%rcx\n");
    emit("    rep movsb\n");
    emit("    mov %%r12, %%rdi\n");
    emit("    call ducky_strlen\n");           /* ... then right + its NUL */
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
    emit("\nducky_str_int:\n");
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
    emit("    call ducky_alloc\n");
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
    emit("\nducky_str_bool:\n");
    emit("    test %%rdi, %%rdi\n");
    emit("    jz .Ltb_false\n");
    emit("    lea .Lstr_true(%%rip), %%rax\n");
    emit("    ret\n");
    emit(".Ltb_false:\n");
    emit("    lea .Lstr_false(%%rip), %%rax\n");
    emit("    ret\n");

    /* input_line() -> one line from stdin without the newline. It reads one
     * byte at a time so the next call starts exactly at the next line. */
    emit("\nducky_input:\n");
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
    emit("    call ducky_alloc\n");
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

    /* scan_int(s: string) -> int: C's atoi on a Ducky string. Leading
     * whitespace is skipped, an optional sign is honoured, digits stop at
     * the first non-digit; without digits the result is 0 and an
     * overflowing value clamps to +-INT64_MAX. */
    emit("\nducky_scan_int:\n");
    emit("    push %%rbx\n");
    emit("    push %%r12\n");
    emit("    push %%r13\n");
    emit("    push %%r14\n");
    emit("    mov %%rdi, %%rbx\n");        /* cursor */
    emit("    xor %%r12d, %%r12d\n");      /* accumulator */
    emit("    xor %%r13d, %%r13d\n");      /* sign: 0 = '+', 1 = '-' */
    emit("    xor %%r14d, %%r14d\n");      /* digits seen */
    emit(".Lsci_ws:\n");
    emit("    movzbl (%%rbx), %%eax\n");
    emit("    cmp $32, %%al\n");           /* ' ' */
    emit("    je .Lsci_wsm\n");
    emit("    cmp $9, %%al\n");            /* \t */
    emit("    je .Lsci_wsm\n");
    emit("    cmp $10, %%al\n");           /* \n */
    emit("    je .Lsci_wsm\n");
    emit("    cmp $11, %%al\n");           /* \v */
    emit("    je .Lsci_wsm\n");
    emit("    cmp $12, %%al\n");           /* \f */
    emit("    je .Lsci_wsm\n");
    emit("    cmp $13, %%al\n");           /* \r */
    emit("    je .Lsci_wsm\n");
    emit("    jmp .Lsci_sign\n");
    emit(".Lsci_wsm:\n");
    emit("    inc %%rbx\n");
    emit("    jmp .Lsci_ws\n");
    emit(".Lsci_sign:\n");
    emit("    cmp $45, %%al\n");           /* '-' */
    emit("    jne .Lsci_plus\n");
    emit("    mov $1, %%r13d\n");
    emit("    inc %%rbx\n");
    emit("    jmp .Lsci_digit\n");
    emit(".Lsci_plus:\n");
    emit("    cmp $43, %%al\n");           /* '+' */
    emit("    jne .Lsci_digit\n");
    emit("    inc %%rbx\n");
    emit(".Lsci_digit:\n");
    emit("    movzbl (%%rbx), %%eax\n");
    emit("    cmp $48, %%al\n");
    emit("    jb .Lsci_end\n");
    emit("    cmp $57, %%al\n");
    emit("    ja .Lsci_end\n");
    emit("    sub $48, %%eax\n");          /* digit, zero-extended in %rax */
    emit("    movabs $922337203685477580, %%rcx\n"); /* INT64_MAX / 10 */
    emit("    cmp %%rcx, %%r12\n");
    emit("    ja .Lsci_clamp\n");
    emit("    jb .Lsci_acc\n");
    emit("    cmp $7, %%eax\n");           /* INT64_MAX % 10 */
    emit("    ja .Lsci_clamp\n");
    emit(".Lsci_acc:\n");
    emit("    imul $10, %%r12, %%r12\n");
    emit("    add %%rax, %%r12\n");
    emit("    inc %%r14\n");
    emit("    inc %%rbx\n");
    emit("    jmp .Lsci_digit\n");
    emit(".Lsci_clamp:\n");
    emit("    movabs $9223372036854775807, %%r12\n");
    emit(".Lsci_clamp_next:\n");
    emit("    inc %%rbx\n");
    emit("    movzbl (%%rbx), %%eax\n");
    emit("    cmp $48, %%al\n");
    emit("    jb .Lsci_end\n");
    emit("    cmp $57, %%al\n");
    emit("    ja .Lsci_end\n");
    emit("    jmp .Lsci_clamp_next\n");
    emit(".Lsci_end:\n");
    emit("    test %%r14, %%r14\n");
    emit("    jz .Lsci_zero\n");           /* no digits: atoi yields 0 */
    emit("    test %%r13, %%r13\n");
    emit("    jz .Lsci_pos\n");
    emit("    neg %%r12\n");
    emit(".Lsci_pos:\n");
    emit("    mov %%r12, %%rax\n");
    emit("    jmp .Lsci_ret\n");
    emit(".Lsci_zero:\n");
    emit("    xor %%eax, %%eax\n");
    emit(".Lsci_ret:\n");
    emit("    pop %%r14\n");
    emit("    pop %%r13\n");
    emit("    pop %%r12\n");
    emit("    pop %%rbx\n");
    emit("    ret\n");

    /* scan_float(s: string) -> double bits: C's atof on a Ducky string.
     * Skips whitespace, takes a sign, accepts inf/nan prefixes, keeps 15
     * significant digits and applies the decimal exponent; without digits
     * the result is 0.0 and trailing junk is ignored. */
    emit("\nducky_scan_float:\n");
    emit("    push %%rbx\n");
    emit("    push %%r12\n");
    emit("    push %%r13\n");
    emit("    push %%r14\n");
    emit("    push %%r15\n");
    emit("    mov %%rdi, %%rbx\n");        /* cursor */
    emit("    xor %%r12d, %%r12d\n");      /* mantissa (at most 15 digits) */
    emit("    xor %%r13d, %%r13d\n");      /* sign: 0 = '+', 1 = '-' */
    emit("    xor %%r14d, %%r14d\n");      /* flags | digits << 8:
                                            * bit0 seen, bit1 started,
                                            * bit2 after the decimal point */
    emit("    xor %%r15d, %%r15d\n");      /* e_base = int digits - skipped zeros */
    emit(".Lscf_ws:\n");
    emit("    movzbl (%%rbx), %%eax\n");
    emit("    cmp $32, %%al\n");
    emit("    je .Lscf_wsm\n");
    emit("    cmp $9, %%al\n");
    emit("    je .Lscf_wsm\n");
    emit("    cmp $10, %%al\n");
    emit("    je .Lscf_wsm\n");
    emit("    cmp $11, %%al\n");
    emit("    je .Lscf_wsm\n");
    emit("    cmp $12, %%al\n");
    emit("    je .Lscf_wsm\n");
    emit("    cmp $13, %%al\n");
    emit("    je .Lscf_wsm\n");
    emit("    jmp .Lscf_sign\n");
    emit(".Lscf_wsm:\n");
    emit("    inc %%rbx\n");
    emit("    jmp .Lscf_ws\n");
    emit(".Lscf_sign:\n");
    emit("    cmp $45, %%al\n");           /* '-' */
    emit("    jne .Lscf_plus\n");
    emit("    mov $1, %%r13d\n");
    emit("    inc %%rbx\n");
    emit("    jmp .Lscf_special\n");
    emit(".Lscf_plus:\n");
    emit("    cmp $43, %%al\n");           /* '+' */
    emit("    jne .Lscf_special\n");
    emit("    inc %%rbx\n");
    /* inf / nan: matched one byte at a time so the scan never reads past
     * the terminating NUL. */
    emit(".Lscf_special:\n");
    emit("    movzbl (%%rbx), %%eax\n");
    emit("    or $32, %%eax\n");           /* fold to lower case */
    emit("    cmp $105, %%eax\n");         /* 'i' */
    emit("    jne .Lscf_trynan\n");
    emit("    movzbl 1(%%rbx), %%eax\n");
    emit("    or $32, %%eax\n");
    emit("    cmp $110, %%eax\n");         /* 'n' */
    emit("    jne .Lscf_trynan\n");
    emit("    movzbl 2(%%rbx), %%eax\n");
    emit("    or $32, %%eax\n");
    emit("    cmp $102, %%eax\n");         /* 'f' */
    emit("    jne .Lscf_trynan\n");
    emit("    movabs $0x7FF0000000000000, %%rax\n");
    emit("    jmp .Lscf_spec_done\n");
    emit(".Lscf_trynan:\n");
    emit("    movzbl (%%rbx), %%eax\n");
    emit("    or $32, %%eax\n");
    emit("    cmp $110, %%eax\n");         /* 'n' */
    emit("    jne .Lscf_mant\n");
    emit("    movzbl 1(%%rbx), %%eax\n");
    emit("    or $32, %%eax\n");
    emit("    cmp $97, %%eax\n");          /* 'a' */
    emit("    jne .Lscf_mant\n");
    emit("    movzbl 2(%%rbx), %%eax\n");
    emit("    or $32, %%eax\n");
    emit("    cmp $110, %%eax\n");         /* 'n' */
    emit("    jne .Lscf_mant\n");
    emit("    movabs $0x7FF8000000000000, %%rax\n");
    emit(".Lscf_spec_done:\n");
    emit("    test %%r13d, %%r13d\n");
    emit("    jz .Lscf_done\n");
    emit("    movabs $0x8000000000000000, %%rdx\n");
    emit("    xor %%rdx, %%rax\n");
    emit("    jmp .Lscf_done\n");
    /* --- mantissa: digits, one decimal point, skipped leading zeros --- */
    emit(".Lscf_mant:\n");
    emit("    movzbl (%%rbx), %%eax\n");
    emit("    cmp $46, %%al\n");           /* '.' */
    emit("    je .Lscf_dot\n");
    emit("    cmp $48, %%al\n");
    emit("    jb .Lscf_tryexp\n");
    emit("    cmp $57, %%al\n");
    emit("    ja .Lscf_tryexp\n");
    emit("    sub $48, %%eax\n");          /* digit, zero-extended in %rax */
    emit("    or $1, %%r14b\n");           /* digits seen */
    emit("    test $4, %%r14b\n");
    emit("    jnz .Lscf_frac\n");
    emit("    inc %%r15\n");               /* integer digit: e_base++ */
    emit(".Lscf_frac:\n");
    emit("    test $2, %%r14b\n");         /* mantissa started? */
    emit("    jnz .Lscf_acc\n");
    emit("    test %%eax, %%eax\n");
    emit("    jz .Lscf_leadzero\n");
    emit("    or $2, %%r14b\n");           /* first significant digit */
    emit("    mov %%rax, %%r12\n");
    emit("    add $256, %%r14\n");         /* digits = 1 */
    emit("    inc %%rbx\n");
    emit("    jmp .Lscf_mant\n");
    emit(".Lscf_leadzero:\n");
    emit("    dec %%r15\n");               /* skipped zero: e_base-- */
    emit("    inc %%rbx\n");
    emit("    jmp .Lscf_mant\n");
    emit(".Lscf_acc:\n");
    emit("    mov %%r14, %%rcx\n");
    emit("    shr $8, %%rcx\n");           /* significant digits kept */
    emit("    cmp $15, %%rcx\n");
    emit("    jae .Lscf_drop\n");          /* only 15 digits fit */
    emit("    imul $10, %%r12, %%r12\n");
    emit("    add %%rax, %%r12\n");
    emit("    add $256, %%r14\n");
    emit(".Lscf_drop:\n");
    emit("    inc %%rbx\n");
    emit("    jmp .Lscf_mant\n");
    emit(".Lscf_dot:\n");
    emit("    test $4, %%r14b\n");
    emit("    jnz .Lscf_end\n");           /* second '.': end of number */
    emit("    or $4, %%r14b\n");
    emit("    inc %%rbx\n");
    emit("    jmp .Lscf_mant\n");
    /* --- exponent: 'e' / 'E', optional sign, at least one digit --- */
    emit(".Lscf_tryexp:\n");
    emit("    or $32, %%eax\n");           /* %eax still holds the raw byte */
    emit("    cmp $101, %%eax\n");         /* 'e' */
    emit("    jne .Lscf_end\n");
    emit("    lea 1(%%rbx), %%rsi\n");
    emit("    xor %%edx, %%edx\n");        /* exponent sign: 0 '+', 1 '-' */
    emit("    movzbl (%%rsi), %%eax\n");
    emit("    cmp $45, %%al\n");
    emit("    jne .Lscf_esign\n");
    emit("    mov $1, %%edx\n");
    emit("    inc %%rsi\n");
    emit("    jmp .Lscf_edigit\n");
    emit(".Lscf_esign:\n");
    emit("    cmp $43, %%al\n");
    emit("    jne .Lscf_edigit\n");
    emit("    inc %%rsi\n");
    emit(".Lscf_edigit:\n");
    emit("    movzbl (%%rsi), %%eax\n");
    emit("    cmp $48, %%al\n");
    emit("    jb .Lscf_end\n");            /* 'e' alone: not part of it */
    emit("    cmp $57, %%al\n");
    emit("    ja .Lscf_end\n");
    emit("    xor %%ecx, %%ecx\n");        /* exponent value */
    emit(".Lscf_eloop:\n");
    emit("    movzbl (%%rsi), %%eax\n");
    emit("    cmp $48, %%al\n");
    emit("    jb .Lscf_eapply\n");
    emit("    cmp $57, %%al\n");
    emit("    ja .Lscf_eapply\n");
    emit("    sub $48, %%eax\n");
    emit("    imul $10, %%rcx, %%rcx\n");
    emit("    add %%rax, %%rcx\n");
    emit("    cmp $400, %%ecx\n");         /* saturate; any value past the */
    emit("    jle .Lscf_econt\n");         /* thresholds lands the same way */
    emit("    mov $400, %%ecx\n");
    emit(".Lscf_econt:\n");
    emit("    inc %%rsi\n");
    emit("    jmp .Lscf_eloop\n");
    emit(".Lscf_eapply:\n");
    emit("    test %%edx, %%edx\n");
    emit("    jz .Lscf_eok\n");
    emit("    neg %%rcx\n");
    emit(".Lscf_eok:\n");
    emit("    mov %%rsi, %%rbx\n");        /* commit the cursor */
    emit("    jmp .Lscf_eval\n");
    emit(".Lscf_end:\n");
    emit("    xor %%ecx, %%ecx\n");        /* no exponent */
    /* --- value = mantissa * 10^(e_base - digits + exponent) --- */
    emit(".Lscf_eval:\n");
    emit("    test $1, %%r14b\n");         /* any digit at all? */
    emit("    jz .Lscf_fail\n");           /* no conversion: 0.0 */
    emit("    test %%r12, %%r12\n");
    emit("    jz .Lscf_zero\n");           /* mantissa is zero: +-0.0 */
    emit("    mov %%r15, %%rsi\n");
    emit("    mov %%r14, %%rdx\n");
    emit("    shr $8, %%rdx\n");
    emit("    sub %%rdx, %%rsi\n");
    emit("    add %%rcx, %%rsi\n");        /* decimal exponent in %rsi */
    emit("    cmp $309, %%rsi\n");
    emit("    jge .Lscf_inf\n");
    emit("    cmp $-340, %%rsi\n");
    emit("    jle .Lscf_zero\n");
    emit("    cvtsi2sd %%r12, %%xmm0\n");
    emit("    cmp $0, %%rsi\n");
    emit("    jge .Lscf_pos_exp\n");
    emit("    cmp $-308, %%rsi\n");
    emit("    jl .Lscf_neg_split\n");
    emit("    neg %%rsi\n");               /* 10^-e, index 1..308 */
    emit("    lea .Lipow10(%%rip), %%rdx\n");
    emit("    mulsd (%%rdx,%%rsi,8), %%xmm0\n");
    emit("    jmp .Lscf_bits\n");
    emit(".Lscf_neg_split:\n");            /* e < -308: split the scaling so
                                            * a subnormal product survives */
    emit("    lea .Lipow10(%%rip), %%rdx\n");
    emit("    mulsd 2464(%%rdx), %%xmm0\n");   /* 10^-308 (308 * 8) */
    emit("    neg %%rsi\n");
    emit("    sub $308, %%rsi\n");         /* index 1..31 */
    emit("    mulsd (%%rdx,%%rsi,8), %%xmm0\n");
    emit("    jmp .Lscf_bits\n");
    emit(".Lscf_pos_exp:\n");
    emit("    lea .Lpow10(%%rip), %%rdx\n");
    emit("    mulsd (%%rdx,%%rsi,8), %%xmm0\n");
    emit(".Lscf_bits:\n");
    emit("    movq %%xmm0, %%rax\n");
    emit("    test %%r13d, %%r13d\n");
    emit("    jz .Lscf_done\n");
    emit("    movabs $0x8000000000000000, %%rdx\n");
    emit("    xor %%rdx, %%rax\n");
    emit("    jmp .Lscf_done\n");
    emit(".Lscf_inf:\n");
    emit("    movabs $0x7FF0000000000000, %%rax\n");
    emit("    test %%r13d, %%r13d\n");
    emit("    jz .Lscf_done\n");
    emit("    movabs $0x8000000000000000, %%rdx\n");
    emit("    xor %%rdx, %%rax\n");
    emit("    jmp .Lscf_done\n");
    emit(".Lscf_zero:\n");
    emit("    xor %%eax, %%eax\n");        /* +0.0 */
    emit("    test %%r13d, %%r13d\n");
    emit("    jz .Lscf_done\n");
    emit("    movabs $0x8000000000000000, %%rax\n"); /* -0.0 */
    emit("    jmp .Lscf_done\n");
    emit(".Lscf_fail:\n");                 /* no conversion: always +0.0 */
    emit("    xor %%eax, %%eax\n");
    emit(".Lscf_done:\n");
    emit("    pop %%r15\n");
    emit("    pop %%r14\n");
    emit("    pop %%r13\n");
    emit("    pop %%r12\n");
    emit("    pop %%rbx\n");
    emit("    ret\n");

    /* A bounds check failed: explain and stop (like the OOM path). */
    emit("\nducky_oob:\n");
    emit("    lea .Loobmsg(%%rip), %%rsi\n");
    emit("    mov $27, %%edx\n");
    emit("    mov $2, %%edi\n");
    emit("    mov $1, %%eax\n");
    emit("    syscall\n");
    emit("    mov $127, %%edi\n");
    emit("    mov $60, %%eax\n");
    emit("    syscall\n");

    /* str_at(s, i) -> one-byte heap string holding s[i]; checks the
     * range first (negative indexes wrap high and are caught too). */
    emit("\nducky_str_at:\n");
    emit("    push %%rbx\n");
    emit("    push %%r12\n");
    emit("    push %%r13\n");                 /* also aligns for the call */
    emit("    mov %%rdi, %%rbx\n");           /* s */
    emit("    mov %%rsi, %%r12\n");           /* index */
    emit("    xor %%eax, %%eax\n");
    emit(".Lsa_len:\n");
    emit("    cmpb $0, (%%rbx, %%rax)\n");
    emit("    je .Lsa_done\n");
    emit("    inc %%rax\n");
    emit("    jmp .Lsa_len\n");
    emit(".Lsa_done:\n");
    emit("    cmp %%rax, %%r12\n");           /* index - len (unsigned) */
    emit("    jae ducky_oob\n");
    emit("    mov $2, %%rdi\n");
    emit("    call ducky_alloc\n");
    emit("    movb (%%rbx, %%r12), %%cl\n");
    emit("    movb %%cl, (%%rax)\n");
    emit("    movb $0, 1(%%rax)\n");
    emit("    pop %%r13\n");
    emit("    pop %%r12\n");
    emit("    pop %%rbx\n");
    emit("    ret\n");

    /* push(array, value) -> a new array with the value appended. The old
     * array is left untouched, so existing references keep their data. */
    emit("\nducky_push:\n");
    emit("    push %%rbx\n");
    emit("    push %%r12\n");
    emit("    push %%r13\n");
    emit("    mov %%rdi, %%rbx\n");           /* old array */
    emit("    mov %%rsi, %%r12\n");           /* value */
    emit("    mov (%%rbx), %%r13\n");         /* count */
    emit("    lea 16(, %%r13, 8), %%rdi\n");  /* 8 + 8 * (count + 1) */
    emit("    call ducky_alloc\n");
    emit("    lea 8(, %%r13, 8), %%rcx\n");   /* bytes of the old block */
    emit("    mov %%rbx, %%rsi\n");
    emit("    mov %%rax, %%rdi\n");
    emit("    push %%rax\n");                 /* remember the new block */
    emit("    cld\n");
    emit("    rep movsb\n");
    emit("    pop %%rax\n");
    emit("    lea 1(%%r13), %%rcx\n");
    emit("    mov %%rcx, (%%rax)\n");         /* count + 1 */
    emit("    mov %%r12, 8(%%rax, %%r13, 8)\n");
    emit("    pop %%r13\n");
    emit("    pop %%r12\n");
    emit("    pop %%rbx\n");
    emit("    ret\n");

    /* fmt_float(bits, buf) -> length. Writes the shortest-ish decimal
     * form: 15 significant digits with rounding, trailing zeros trimmed,
     * exponent notation outside [1e-15, 1e18]. NaN -> "nan",
     * infinity -> "inf"/"-inf", zero -> "0". */
    emit("\nducky_fmt_float:\n");
    emit("    push %%rbp\n");
    emit("    mov %%rsp, %%rbp\n");
    emit("    push %%rbx\n");
    emit("    push %%r12\n");
    emit("    push %%r13\n");
    emit("    push %%r14\n");
    emit("    push %%r15\n");
    emit("    sub $32, %%rsp\n");
    emit("    lea -72(%%rbp), %%rbx\n");      /* 16 digit bytes live here */
    emit("    mov %%rsi, %%r13\n");           /* output base */
    emit("    mov %%rsi, %%r14\n");           /* output cursor */
    emit("    movq %%rdi, %%xmm0\n");         /* the value */
    emit("    mov %%rdi, %%rax\n");

    /* NaN / Inf share exponent 0x7FF. */
    emit("    mov %%rax, %%rcx\n");
    emit("    shr $52, %%rcx\n");
    emit("    and $0x7FF, %%rcx\n");
    emit("    cmp $0x7FF, %%rcx\n");
    emit("    je .Lff_special\n");

    /* Zero: only the sign bit set (bits << 1 becomes 0). */
    emit("    mov %%rax, %%rcx\n");
    emit("    shl $1, %%rcx\n");
    emit("    jz .Lff_zero\n");

    /* Sign: print '-' and clear the sign bit. */
    emit("    mov %%rax, %%r15\n");
    emit("    shr $63, %%r15\n");
    emit("    test %%r15d, %%r15d\n");
    emit("    jz .Lff_pos\n");
    emit("    movb $45, (%%r14)\n");
    emit("    inc %%r14\n");
    emit("    movabs $0x7FFFFFFFFFFFFFFF, %%rdx\n");
    emit("    and %%rdx, %%rax\n");          /* |bits| */
    emit("    movq %%rax, %%xmm0\n");
    emit(".Lff_pos:\n");

    /* Normalize into [1, 10) and count the decimal exponent. */
    emit("    xor %%r12d, %%r12d\n");
    emit(".Lff_norm:\n");
    emit("    comisd .Ldten(%%rip), %%xmm0\n");
    emit("    jae .Lff_div\n");
    emit("    comisd .Ldfone(%%rip), %%xmm0\n");
    emit("    jb .Lff_mul\n");
    emit("    jmp .Lff_digits\n");
    emit(".Lff_div:\n");
    emit("    movsd .Ldten(%%rip), %%xmm1\n");
    emit("    divsd %%xmm1, %%xmm0\n");
    emit("    inc %%r12\n");
    emit("    jmp .Lff_norm\n");
    emit(".Lff_mul:\n");
    emit("    movsd .Ldten(%%rip), %%xmm1\n");
    emit("    mulsd %%xmm1, %%xmm0\n");
    emit("    dec %%r12\n");
    emit("    jmp .Lff_norm\n");

    /* 15 significant digits plus one guard digit for rounding. */
    emit(".Lff_digits:\n");
    emit("    cvttsd2si %%xmm0, %%rax\n");
    emit("    cvtsi2sd %%rax, %%xmm1\n");
    emit("    subsd %%xmm1, %%xmm0\n");
    emit("    add $48, %%al\n");
    emit("    mov %%al, (%%rbx)\n");
    emit("    mov $1, %%ecx\n");
    emit(".Lff_frac:\n");
    emit("    mulsd .Ldten(%%rip), %%xmm0\n");
    emit("    cvttsd2si %%xmm0, %%rax\n");
    emit("    cvtsi2sd %%rax, %%xmm1\n");
    emit("    subsd %%xmm1, %%xmm0\n");
    emit("    add $48, %%al\n");
    emit("    mov %%al, (%%rbx, %%rcx)\n");
    emit("    inc %%ecx\n");
    emit("    cmp $16, %%ecx\n");
    emit("    jl .Lff_frac\n");

    /* Round up on the guard digit, carrying left through the 9s. */
    emit("    cmpb $53, 15(%%rbx)\n");
    emit("    jb .Lff_round_done\n");
    emit("    mov $14, %%ecx\n");
    emit(".Lff_carry:\n");
    emit("    movzbl (%%rbx, %%rcx), %%eax\n");
    emit("    inc %%eax\n");
    emit("    cmp $57, %%eax\n");
    emit("    jbe .Lff_carry_set\n");
    emit("    movb $48, (%%rbx, %%rcx)\n");
    emit("    dec %%ecx\n");
    emit("    jns .Lff_carry\n");
    emit("    movb $49, (%%rbx)\n");          /* rolled over to 10...0 */
    emit("    inc %%r12\n");
    emit("    jmp .Lff_round_done\n");
    emit(".Lff_carry_set:\n");
    emit("    mov %%al, (%%rbx, %%rcx)\n");
    emit(".Lff_round_done:\n");

    /* Plain decimal only while the exponent stays in range. */
    emit("    cmp $17, %%r12d\n");
    emit("    jg .Lff_sci\n");
    emit("    cmp $-15, %%r12d\n");
    emit("    jl .Lff_sci\n");
    emit("    test %%r12d, %%r12d\n");
    emit("    jns .Lff_plain_pos\n");

    /* Plain, negative exponent: 0.00ddd */
    emit(".Lff_plain_neg:\n");
    emit("    movb $48, (%%r14)\n");
    emit("    inc %%r14\n");
    emit("    movb $46, (%%r14)\n");
    emit("    inc %%r14\n");
    emit("    mov %%r12d, %%ecx\n");
    emit("    neg %%ecx\n");
    emit("    dec %%ecx\n");                  /* leading zeros = -exp10-1 */
    emit("    test %%ecx, %%ecx\n");
    emit("    jz .Lff_pn_digits\n");
    emit(".Lff_pn_zeros:\n");
    emit("    movb $48, (%%r14)\n");
    emit("    inc %%r14\n");
    emit("    dec %%ecx\n");
    emit("    jnz .Lff_pn_zeros\n");
    emit(".Lff_pn_digits:\n");
    emit("    mov $14, %%ecx\n");
    emit(".Lff_k2:\n");
    emit("    cmpb $48, (%%rbx, %%rcx)\n");
    emit("    jne .Lff_pn_write\n");
    emit("    dec %%ecx\n");
    emit("    jmp .Lff_k2\n");
    emit(".Lff_pn_write:\n");
    emit("    xor %%r15d, %%r15d\n");
    emit(".Lff_pn_w:\n");
    emit("    movzbl (%%rbx, %%r15), %%eax\n");
    emit("    mov %%al, (%%r14)\n");
    emit("    inc %%r14\n");
    emit("    inc %%r15d\n");
    emit("    cmp %%r15d, %%ecx\n");
    emit("    jge .Lff_pn_w\n");
    emit("    jmp .Lff_done\n");

    /* Plain, non-negative exponent: ddd.ddd (positions past 14 are 0). */
    emit(".Lff_plain_pos:\n");
    emit("    xor %%r15d, %%r15d\n");
    emit(".Lff_pp:\n");
    emit("    cmp $15, %%r15d\n");
    emit("    jge .Lff_pp_zero\n");
    emit("    movzbl (%%rbx, %%r15), %%eax\n");
    emit("    jmp .Lff_pp_store\n");
    emit(".Lff_pp_zero:\n");
    emit("    mov $48, %%eax\n");
    emit(".Lff_pp_store:\n");
    emit("    mov %%al, (%%r14)\n");
    emit("    inc %%r14\n");
    emit("    inc %%r15d\n");
    emit("    cmp %%r15d, %%r12d\n");
    emit("    jge .Lff_pp\n");
    emit("    cmp $14, %%r12d\n");
    emit("    jge .Lff_done\n");
    emit("    lea 1(%%r12), %%r15\n");        /* first fraction position */
    emit("    mov $14, %%ecx\n");
    emit(".Lff_k1:\n");
    emit("    cmp %%r15d, %%ecx\n");
    emit("    jl .Lff_done\n");               /* every fraction digit is 0 */
    emit("    cmpb $48, (%%rbx, %%rcx)\n");
    emit("    je .Lff_k1_dec\n");
    emit("    jmp .Lff_frac_write\n");
    emit(".Lff_k1_dec:\n");
    emit("    dec %%ecx\n");
    emit("    jmp .Lff_k1\n");
    emit(".Lff_frac_write:\n");
    emit("    movb $46, (%%r14)\n");
    emit("    inc %%r14\n");
    emit(".Lff_fw:\n");
    emit("    movzbl (%%rbx, %%r15), %%eax\n");
    emit("    mov %%al, (%%r14)\n");
    emit("    inc %%r14\n");
    emit("    inc %%r15d\n");
    emit("    cmp %%r15d, %%ecx\n");
    emit("    jge .Lff_fw\n");
    emit("    jmp .Lff_done\n");

    /* Exponent notation: d.dddde+NN */
    emit(".Lff_sci:\n");
    emit("    movzbl (%%rbx), %%eax\n");
    emit("    mov %%al, (%%r14)\n");
    emit("    inc %%r14\n");
    emit("    mov $14, %%ecx\n");
    emit(".Lff_k3:\n");
    emit("    cmp $1, %%ecx\n");
    emit("    jl .Lff_sci_exp\n");
    emit("    cmpb $48, (%%rbx, %%rcx)\n");
    emit("    je .Lff_k3_dec\n");
    emit("    jmp .Lff_sci_dot\n");
    emit(".Lff_k3_dec:\n");
    emit("    dec %%ecx\n");
    emit("    jmp .Lff_k3\n");
    emit(".Lff_sci_dot:\n");
    emit("    movb $46, (%%r14)\n");
    emit("    inc %%r14\n");
    emit("    mov $1, %%r15d\n");
    emit(".Lff_sd:\n");
    emit("    movzbl (%%rbx, %%r15), %%eax\n");
    emit("    mov %%al, (%%r14)\n");
    emit("    inc %%r14\n");
    emit("    inc %%r15d\n");
    emit("    cmp %%r15d, %%ecx\n");
    emit("    jge .Lff_sd\n");
    emit(".Lff_sci_exp:\n");
    emit("    movb $101, (%%r14)\n");         /* 'e' */
    emit("    inc %%r14\n");
    emit("    test %%r12d, %%r12d\n");
    emit("    js .Lff_sci_neg\n");
    emit("    movb $43, (%%r14)\n");          /* '+' */
    emit("    inc %%r14\n");
    emit("    jmp .Lff_sci_mag\n");
    emit(".Lff_sci_neg:\n");
    emit("    movb $45, (%%r14)\n");          /* '-' */
    emit("    inc %%r14\n");
    emit("    neg %%r12d\n");
    emit(".Lff_sci_mag:\n");
    emit("    cmp $10, %%r12d\n");
    emit("    jge .Lff_sci_two\n");
    emit("    movb $48, (%%r14)\n");          /* two-digit exponent */
    emit("    inc %%r14\n");
    emit("    mov %%r12d, %%eax\n");
    emit("    add $48, %%eax\n");
    emit("    mov %%al, (%%r14)\n");
    emit("    inc %%r14\n");
    emit("    jmp .Lff_done\n");
    emit(".Lff_sci_two:\n");
    emit("    cmp $100, %%r12d\n");
    emit("    jge .Lff_sci_three\n");
    emit("    mov %%r12d, %%eax\n");
    emit("    mov $10, %%ecx\n");
    emit("    xor %%edx, %%edx\n");
    emit("    div %%ecx\n");
    emit("    add $48, %%al\n");
    emit("    mov %%al, (%%r14)\n");
    emit("    inc %%r14\n");
    emit("    mov %%dl, %%al\n");
    emit("    add $48, %%al\n");
    emit("    mov %%al, (%%r14)\n");
    emit("    inc %%r14\n");
    emit("    jmp .Lff_done\n");
    emit(".Lff_sci_three:\n");
    emit("    mov %%r12d, %%eax\n");
    emit("    mov $100, %%ecx\n");
    emit("    xor %%edx, %%edx\n");
    emit("    div %%ecx\n");
    emit("    add $48, %%al\n");
    emit("    mov %%al, (%%r14)\n");
    emit("    inc %%r14\n");
    emit("    mov %%edx, %%eax\n");
    emit("    mov $10, %%ecx\n");
    emit("    xor %%edx, %%edx\n");
    emit("    div %%ecx\n");
    emit("    add $48, %%al\n");
    emit("    mov %%al, (%%r14)\n");
    emit("    inc %%r14\n");
    emit("    mov %%dl, %%al\n");
    emit("    add $48, %%al\n");
    emit("    mov %%al, (%%r14)\n");
    emit("    inc %%r14\n");
    emit("    jmp .Lff_done\n");

    emit(".Lff_special:\n");
    emit("    mov %%rax, %%rdx\n");
    emit("    shl $12, %%rdx\n");
    emit("    shr $12, %%rdx\n");             /* mantissa */
    emit("    jz .Lff_inf\n");
    emit("    movb $110, (%%r14)\n");         /* 'n' */
    emit("    inc %%r14\n");
    emit("    movb $97, (%%r14)\n");          /* 'a' */
    emit("    inc %%r14\n");
    emit("    movb $110, (%%r14)\n");         /* 'n' */
    emit("    inc %%r14\n");
    emit("    jmp .Lff_done\n");
    emit(".Lff_inf:\n");
    emit("    test %%rax, %%rax\n");
    emit("    jns .Lff_inf_body\n");
    emit("    movb $45, (%%r14)\n");
    emit("    inc %%r14\n");
    emit(".Lff_inf_body:\n");
    emit("    movb $105, (%%r14)\n");         /* 'i' */
    emit("    inc %%r14\n");
    emit("    movb $110, (%%r14)\n");         /* 'n' */
    emit("    inc %%r14\n");
    emit("    movb $102, (%%r14)\n");         /* 'f' */
    emit("    inc %%r14\n");
    emit("    jmp .Lff_done\n");
    emit(".Lff_zero:\n");
    emit("    movb $48, (%%r14)\n");
    emit("    inc %%r14\n");

    emit(".Lff_done:\n");
    emit("    mov %%r14, %%rax\n");
    emit("    sub %%r13, %%rax\n");
    emit("    add $32, %%rsp\n");
    emit("    pop %%r15\n");
    emit("    pop %%r14\n");
    emit("    pop %%r13\n");
    emit("    pop %%r12\n");
    emit("    pop %%rbx\n");
    emit("    pop %%rbp\n");
    emit("    ret\n");

    /* serve(value: float) -> writes the decimal form and a newline */
    emit("\nducky_serve_float:\n");
    emit("    push %%rbp\n");
    emit("    mov %%rsp, %%rbp\n");
    emit("    sub $80, %%rsp\n");
    emit("    lea -80(%%rbp), %%rsi\n");      /* buffer (rdi = bits) */
    emit("    call ducky_fmt_float\n");
    emit("    mov %%rax, %%rdx\n");
    emit("    lea -80(%%rbp), %%rsi\n");
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

    /* str(value: float) -> the decimal form on the heap */
    emit("\nducky_str_float:\n");
    emit("    push %%rbp\n");
    emit("    mov %%rsp, %%rbp\n");
    emit("    push %%rbx\n");                 /* %rbx holds the length */
    emit("    sub $88, %%rsp\n");
    emit("    lea -88(%%rbp), %%rsi\n");      /* buffer (rdi = bits) */
    emit("    call ducky_fmt_float\n");
    emit("    mov %%rax, %%rbx\n");
    emit("    lea 1(%%rbx), %%rdi\n");        /* + 1 for the NUL */
    emit("    call ducky_alloc\n");
    emit("    mov %%rax, %%rdi\n");
    emit("    lea -88(%%rbp), %%rsi\n");
    emit("    mov %%rbx, %%rcx\n");
    emit("    cld\n");
    emit("    rep movsb\n");
    emit("    movb $0, (%%rax, %%rbx)\n");
    emit("    add $88, %%rsp\n");
    emit("    pop %%rbx\n");
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

    /* Extern declarations pull in the C library at link time. */
    g_libc = 0;
    for (int i = 0; i < prog->nfuncs; i++) {
        if (prog->funcs[i]->is_extern) {
            g_libc = 1;
            break;
        }
    }

    emit("/* Generated by duckyc %s from %s - do not edit. */\n", DUCKYC_VERSION, src->path);
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
    emit(".Loommsg:\n    .ascii \"ducky: out of memory\\n\"\n");
    emit(".Loobmsg:\n    .ascii \"ducky: index out of bounds\\n\"\n");
    emit(".Ldten:\n    .quad 0x4024000000000000\n");  /* 10.0 */
    emit(".Ldfone:\n    .quad 0x3FF0000000000000\n"); /* 1.0  */
    /* Powers of ten for scan_float: .Lpow10[i] = 10^i, .Lipow10[i] = 10^-i.
     * The bit patterns are computed here with strtod so the tables match
     * the host's rounding exactly. */
    emit(".Lpow10:\n");
    for (int i = 0; i <= 308; i++) {
        char buf[32];
        unsigned long long bits;
        double v;
        snprintf(buf, sizeof buf, "1e%d", i);
        v = strtod(buf, NULL);
        memcpy(&bits, &v, sizeof bits);
        emit("    .quad 0x%016llx\n", bits);
    }
    emit(".Lipow10:\n");
    for (int i = 0; i <= 340; i++) {
        char buf[32];
        unsigned long long bits;
        double v;
        snprintf(buf, sizeof buf, "1e-%d", i);
        v = strtod(buf, NULL);
        memcpy(&bits, &v, sizeof bits);
        emit("    .quad 0x%016llx\n", bits);
    }
    for (int i = 0; i < nstrs; i++) {
        emit("%s:\n    .string \"", strs[i].label);
        emit_string_bytes(strs[i].text);
        emit("\"\n");
    }

    emit("\n    .section .bss\n");
    emit("    .align 8\n");
    emit("ducky_brk:\n    .zero 8\n");
}
