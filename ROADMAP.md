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
| L4 | `nest` declarations with inference or annotation, assignment, block scopes + shadowing | done |
| L5 | Expressions with full precedence: `\|\|` `&&` `==` `!=` `<` `<=` `>` `>=` `+` `-` `*` `/` `%`, unary `-` `!`, parentheses | done |
| L6 | Control flow: `when` / `otherwise when` / `otherwise`, `while` | done |
| L7 | Functions: any arity (>6 via stack), `-> type`, recursion, mutual recursion, definite-return analysis | done |
| L8 | Entry point `wing main() -> int`, return value = exit status | done |
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

**Update v0.2.0 (breaking):** the user-facing keywords are now Duck-native —
`fn` → `wing`, `let` → `nest`, `if` → `when`, `else` → `otherwise`,
`return` → `send`, `print` → `serve`. The old words are no longer keywords
(identifiers such as `wingman` keep working) and old syntax is rejected with
a hint. The suite grew to **30 tests (12 runtime + 18 error), all passing.**

---

## Milestone M1 — Next language features

| # | Requirement | Notes |
|---|---|---|
| N1 | `for` loops (`for i in 0..10`) | desugars to `while` |
| N2 | Floats (`f64`) | new codegen paths, runtime number formatting |
| N3 | Arrays (`[int]`) with `len`, indexing, bounds | needs runtime allocation (brk/mmap) |
| N4 | String operations: `+` concatenation, `len`, indexing | |
| N5 | Standard input: `input_line()` reading from stdin | `read` syscall |
| N6 | Multiple source files / `import` | driver links several `.o` |
| N7 | Structs and field access | |
| N8 | `break` / `continue` | |
| N9 | Global constants (`const`) | |
| N10 | Better errors: multiple errors per run instead of fail-fast | recovery in parser + sema |
| N11 | Publish the extension to Open VSX and the VS Code Marketplace | needs a publisher account |
| N12 | File icon for `.duck` files in the explorer | done — declared as the language icon (light/dark); works with themes that have specific file icons (e.g. the default Seti) without replacing them |

## Milestone M2 — Engineering

| # | Requirement | Notes |
|---|---|---|
| E1 | Peephole optimizer (constant folding, redundant mov elimination) | |
| E2 | Register allocation instead of the push-machine evaluator | |
| E3 | Debug info (DWARF line tables) for gdb | |
| E4 | Warnings: unused variables, unreachable code | |
| E5 | Fuzzing / property tests for the lexer and parser | |
| E6 | CI script running `make clean && make && make test` | |

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
| B2 | Bitwise operators `&` `\|` `^` `~` `<<` `>>` | registers, flags, masks |
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
