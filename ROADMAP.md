# Duck — Requirements & Roadmap

This file tracks the requirements of the project: what the initial milestone
delivered and what the next releases target.

---

## Milestone M0 — Initial release (DONE, v0.1.0)

The goal: *a real language that compiles to machine code*, with its syntax and
compiler built from scratch.

### Toolchain

| # | Requirement | Status |
|---|---|---|
| T1 | Compiler written in C11, builds with `make`, zero warnings under `-Wall -Wextra` | done |
| T2 | Pipeline: lexer → parser → semantic analysis → code generator | done |
| T3 | Emit x86-64 assembly (AT&T syntax, System V AMD64 ABI) | done |
| T4 | Assemble with GNU `as`, link with `ld`, produce a native ELF executable | done |
| T5 | `duckc` CLI: `-o`, `-S/--emit-asm`, `--dump-tokens`, `--version`, `--help` | done |
| T6 | Freestanding output: `_start`, raw `write`/`exit` syscalls, no libc | done |
| T7 | Error reporting with `file:line:col`, source excerpt and caret | done |
| T8 | Installer: `install.sh` (one-liner via curl, `--user`/`--prefix`/`--no-editor`/`--uninstall`) that installs the compiler **and** the editor extension; `make install`/`uninstall` do the same | done |
| T9 | Updater: `update.sh`, installed as the `duck-update` command (`--check` compares the stamped commit with `origin/main`, `git pull --ff-only` from a checkout, curl-able from anywhere); `make update` | done |

### Language

| # | Requirement | Status |
|---|---|---|
| L1 | Lexical structure: comments (`//`, `/* */`), identifiers, keywords | done |
| L2 | Literals: decimal/hex `int`, `bool`, `string` with escapes (`\n \t \r \\ \"`) | done |
| L3 | Types: `int` (64-bit), `bool`, `string`; `void` for calls only; no implicit conversions | done |
| L4 | `let` declarations with inference or annotation, assignment, block scopes + shadowing | done |
| L5 | Expressions with full precedence: `\|\|` `&&` `==` `!=` `<` `<=` `>` `>=` `+` `-` `*` `/` `%`, unary `-` `!`, parentheses | done |
| L6 | Control flow: `if` / `else if` / `else`, `while` | done |
| L7 | Functions: any arity (>6 via stack), `-> type`, recursion, mutual recursion, definite-return analysis | done |
| L8 | Entry point `fn main() -> int`, return value = exit status | done |
| L9 | Built-in `serve` for `int`, `bool`, `string` (newline-terminated) | done |
| L10 | String equality by content (`==`, `!=`) | done |
| L11 | Short-circuit evaluation of `&&` and `\|\|` | done |
| L12 | Formal grammar (EBNF) and semantics written down | done — `SPEC.md` |

### Quality

| # | Requirement | Status |
|---|---|---|
| Q1 | Test harness comparing stdout of compiled programs against expected files | done |
| Q2 | Negative tests: each diagnostic class has a test proving it fires | done |
| Q3 | Coverage of: precedence, division/modulo signs, short-circuit (no side effect), >6-argument ABI, deep + mutual recursion, scopes, exit codes | done |
| Q4 | Example programs that compile out of the box (`make examples`) | done |
| Q5 | README + full language specification | done |
| Q6 | Editor support: VS Code/VSCodium/Cursor extension (TextMate grammar + language configuration), validated with the real TextMate engine | done |

**Test count: 22 (10 runtime + 12 error), all passing.**

**Update v0.2.0 (breaking, superseded):** the keywords were renamed to
Duck-native words (`fn` → `wing`, `let` → `nest`, `if` → `when`, `else` →
`otherwise`), keeping `send` and `serve`. v0.3.0 reverted the four renames to
their English spellings after feedback, so `wing`, `nest`, `when` and
`otherwise` are ordinary identifiers again (with a hint when used as syntax);
`send` / `serve` remain. The suite is now **42 tests (17 runtime + 25 error),
all passing.**

---

## Milestone M1 — Next language features

| # | Requirement | Notes |
|---|---|---|
| N1 | `for` loops (`for i in a .. b`) | done — v0.3.0; dedicated codegen (bounds evaluated once into a hidden slot), empty ranges, scoped loop variable |
| N2 | Floats (`f64`) | done — v0.4.0; `float` type + literals (`1.5`, `1e-4`), SSE2 arithmetic/comparisons (IEEE-754, incl. NaN rules), runtime formatter (15 rounded significant digits, exponent outside `[1e-15, 1e18)`, `inf`/`nan`), `float(x)`/`int(x)` conversions |
| N3 | Arrays (`[int]`) with `len`, indexing, bounds | done — v0.4.0; all four element types, `[count][elems…]` heap blocks over the `brk` bump allocator, bounds-checked reads/writes (`duck: index out of bounds`, exit 127), reference semantics, `push()` copy-on-append, nested literals via hidden stack slots |
| N4 | String operations: `+` concatenation, `len`, indexing | done — v0.4.0; `+`/`len()` in v0.3.0, `s[i]` returns a one-byte string (bounds-checked) |
| N5 | Standard input: `input_line()` reading from stdin | done — v0.3.0; `read` syscall, one line per call, EOF → `""` |
| N6 | Multiple source files / `import` | driver links several `.o` |
| N7 | Structs and field access | blocked on a nominal-types refactor |
| N8 | `break` / `continue` | done — v0.3.0; innermost loop at any nesting depth |
| N9 | Global constants (`const`) | done — v0.4.0; top-level `const NAME = <literal>;`, literals only, any declaration order, no storage (references substitute the literal), locals may shadow |
| N10 | Better errors: multiple errors per run instead of fail-fast | recovery in parser + sema |
| N11 | Publish the extension to Open VSX and the VS Code Marketplace | needs a publisher account |
| N12 | File icon for `.duck` files in the explorer | done — declared as the language icon (light/dark); works with themes that have specific file icons (e.g. the default Seti) without replacing them |
| N13 | Bitwise operators `&` `\|` `^` `~` `<<` `>>` | done — v0.3.0; C precedence, arithmetic `>>`, `int` only |
| N14 | `str()` builtin (`int`/`bool` → `string`) | done — v0.3.0; extended to `float` in v0.4.0 |

**Update v0.4.0:** floats, arrays (with string indexing), `const`, `push()`
and the numeric conversions. The suite is now **59 tests (22 runtime + 37
error), all passing.**

## Milestone M2 — Engineering

| # | Requirement | Notes |
|---|---|---|
| E1 | Peephole optimizer (constant folding, redundant mov elimination) | |
| E2 | Register allocation instead of the push-machine evaluator | |
| E3 | Debug info (DWARF line tables) for gdb | |
| E4 | Warnings: unused variables, unreachable code | |
| E5 | Fuzzing / property tests for the lexer and parser | |
| E6 | CI script running `make clean && make && make test` | done — `.github/workflows/ci.yml` (build, test, examples) |

## Milestone M3 — Ambitious

| # | Requirement | Notes |
|---|---|---|
| A1 | Direct machine-code emission (own ELF writer, no `as`/`ld`) | |
| A2 | Self-hosting: rewrite `duckc` in Duck | needs A1… and M1 features |
| A3 | Package manager and standard library | |

## Milestone M4 — Bare metal: a Duck kernel

*Could an operating system be written in Duck?* The foundation already
points that way: `duckc` emits **freestanding** x86-64 binaries (`_start`,
raw syscalls, no libc at all) and owns its own backend, so it does not need
an OS to target a bare machine. What is missing is everything that talks to
hardware directly:

| # | Requirement | Why a kernel needs it |
|---|---|---|
| B1 | Global/static variables at known addresses | kernel state, IDT/GDT, buffers |
| B2 | Bitwise operators `&` `\|` `^` `~` `<<` `>>` | registers, flags, masks — done in v0.3.0 |
| B3 | Pointers, address-of and volatile-correct access | MMIO: VGA text at `0xB8000`, APIC, UART |
| B4 | Structs (builds on N7) | descriptor tables, interrupt frames |
| B5 | Intrinsics or inline assembly: `in`, `out`, `cli`, `sti`, `hlt` | port I/O, interrupt control |
| B6 | Linker script + multiboot2 header, a `duckc --freestanding` target flag | GRUB loads the kernel where it must live |
| B7 | Demo kernel: VGA text “Hello from Duck” booting in QEMU | proves the whole pipeline end to end |
| B8 | Interrupt handling: IDT, ISRs, PIT timer, keyboard input | a kernel that reacts to the world |

With B1–B8 the same compiler that builds programs for Linux today could
build a small teaching/hobby kernel (xv6 / Rust-tutorial league): a real OS
that boots, though deliberately not a production one — no drivers,
networking or MMU policy.
