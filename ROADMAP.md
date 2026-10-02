# Ducky — Requirements & Roadmap

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
| T5 | `duckyc` CLI: `-o`, `-S/--emit-asm`, `--dump-tokens`, `--version`, `--help` | done |
| T6 | Freestanding output: `_start`, raw `write`/`exit` syscalls, no libc | done |
| T7 | Error reporting with `file:line:col`, source excerpt and caret | done |
| T8 | Installer: `install.sh` (one-liner via curl, `--user`/`--prefix`/`--no-editor`/`--uninstall`) that installs the compiler **and** the editor extension; `make install`/`uninstall` do the same | done |
| T9 | Updater: `update.sh`, installed as the `ducky-update` command (`--check` compares the stamped commit with `origin/main`, `git pull --ff-only` from a checkout, curl-able from anywhere); `make update` | done |

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
Ducky-native words (`fn` → `wing`, `let` → `nest`, `if` → `when`, `else` →
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
| N3 | Arrays (`[int]`) with `len`, indexing, bounds | done — v0.4.0; all four element types, `[count][elems…]` heap blocks over the `brk` bump allocator, bounds-checked reads/writes (`ducky: index out of bounds`, exit 127), reference semantics, `push()` copy-on-append, nested literals via hidden stack slots |
| N4 | String operations: `+` concatenation, `len`, indexing | done — v0.4.0; `+`/`len()` in v0.3.0, `s[i]` returns a one-byte string (bounds-checked) |
| N5 | Standard input: `input_line()` reading from stdin | done — v0.3.0; `read` syscall, one line per call, EOF → `""` |
| N6 | Multiple source files / `import` | done — v0.6.0; top-level `import "path.duck";` (new keyword), paths relative to the importing file, the whole import graph is loaded depth first and deduplicated by canonical path (diamonds and cycles load a file once), all files share one global namespace (declarations are merged before analysis, so cross-file calls/structs/constants and cross-file name collisions behave exactly like intra-file ones), every file is parsed with its own tokens so diagnostics name the file they occur in; rejected: `import` outside the top level, missing file, directory, non-string path. Compiles the merged program as one unit rather than linking several `.o` (per-file objects stay a future option) |
| N7 | Structs and field access | done — v0.5.0; `struct Name { f: T, ... }` top-level declarations (comma-separated fields, trailing comma allowed), nominal typing via an interned type registry (`TY_STRUCT_BASE + index`, no full type refactor needed), positional constructors `Point(1, 2)` with arity/type checks, field read/write `p.x` / `p.x = v` (`.` lexes as a new `TK_DOT`), reference semantics, structs as parameters/returns/fields, free declaration order (parser pre-pass interns names), `==`/`serve`/arrays-of-structs rejected, duplicate/reserved/builtin/colliding names rejected |
| N8 | `break` / `continue` | done — v0.3.0; innermost loop at any nesting depth |
| N9 | Global constants (`const`) | done — v0.4.0; top-level `const NAME = <literal>;`, literals only, any declaration order, no storage (references substitute the literal), locals may shadow |
| N10 | Better errors: multiple errors per run instead of fail-fast | recovery in parser + sema |
| N11 | Publish the extension to Open VSX and the VS Code Marketplace | needs a publisher account |
| N12 | File icon for `.duck` files in the explorer | done — declared as the language icon (light/dark); works with themes that have specific file icons (e.g. the default Seti) without replacing them |
| N13 | Bitwise operators `&` `\|` `^` `~` `<<` `>>` | done — v0.3.0; C precedence, arithmetic `>>`, `int` only |
| N14 | `str()` builtin (`int`/`bool` → `string`) | done — v0.3.0; extended to `float` in v0.4.0 |
| N15 | Input parsing builtins: `scan_int` / `scan_float` (+ `_line`) | done — v0.7.0; `scan_int(s)` parses a `string` C `atoi`-style (whitespace, optional sign, decimal only, stops at the first invalid char; no digits → `0`, overflow clamps to ±INT64_MAX) and `scan_float(s)` parses C `atof`-style (15 significant digits, `.` and `e` exponents, `inf`/`nan` accepted case-insensitively, overflow → ±inf, underflow → `0.0`, trailing junk ignored, no digits → `0.0`); `scan_int_line()` / `scan_float_line()` read one line from stdin first; parse failure is never a runtime error |
| N16 | `extern fn ...;` declarations, link against the C library | done — v0.7.0; `extern fn printf(fmt: string, ...) -> int;` ends with `;` and has no body, signature limited to `int`/`float`/`bool`/`string` (plus no return type), variadic `...` with a minimum-arity check and scalar/string extra arguments, System V-correct marshalling (int/bool/string → `rdi…r9` in order, `float` → `xmm0…xmm7` in order, register exhaustion falls back to the stack in argument order, `%al` = vector-register count for variadics), float returns arrive in `xmm0` → `%rax`; the driver links with `cc -nostartfiles -lm` only when the program declares `extern` (pure programs keep the raw `ld` link and stay freestanding) and `_start` flushes stdio before the exit syscall; rejected: `extern fn main`, array/struct in the signature |

**Update v0.4.0:** floats, arrays (with string indexing), `const`, `push()`
and the numeric conversions. The suite is then **59 tests (22 runtime + 37
error), all passing.**

**Update v0.5.0:** structs — declarations, constructors, field access and
assignment, reference semantics, structs everywhere a value can go (plus the
rejections: comparison, `serve`, arrays of structs, name collisions). The
suite is now **78 tests (23 runtime + 55 error), all passing.**

**Update v0.6.0:** multi-file programs — `import` statements, transitive and
deduplicated loading (diamonds and cycles), one global namespace across
files (functions, structs, constants and `main` may live anywhere) and
per-file diagnostics. The suite is now **85 tests (24 runtime + 61 error),
all passing.**

**Update v0.7.0:** the **rename to Ducky** — project and language Ducky,
compiler `duckyc`, runtime prefix `ducky_`, messages `ducky:`, repository
`ducky-lang-com/ducky-lang`, extension `ducky-lang` (the source extension
stays `.duck`), plus the four `scan_*` builtins (N15) and `extern`
declarations with C-library linking (N16). The suite is now **99 tests
(26 runtime + 73 error), all passing.**

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
| A2 | Self-hosting: rewrite `duckyc` in Ducky | needs A1… and M1 features |
| A3 | Package manager and standard library | |

## Milestone M4 — Bare metal: a Ducky kernel

*Could an operating system be written in Ducky?* The foundation already
points that way: `duckyc` emits **freestanding** x86-64 binaries (`_start`,
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
| B6 | Linker script + multiboot2 header, a `duckyc --freestanding` target flag | GRUB loads the kernel where it must live |
| B7 | Demo kernel: VGA text “Hello from Ducky” booting in QEMU | proves the whole pipeline end to end |
| B8 | Interrupt handling: IDT, ISRs, PIT timer, keyboard input | a kernel that reacts to the world |

With B1–B8 the same compiler that builds programs for Linux today could
build a small teaching/hobby kernel (xv6 / Rust-tutorial league): a real OS
that boots, though deliberately not a production one — no drivers,
networking or MMU policy.
