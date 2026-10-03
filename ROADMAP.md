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
| N17 | The `tensor` type, with the **shape in the type** | done — v0.8.0; `tensor[2, 3]` is a distinct type from `tensor[3, 2]`, dims are compile-time integers (literal or top-level `const`), positive, ≤ 1e9, rank ≤ 8; layout identical to `[float]` (`[count][elems…]`, row-major); `tensor(shape, data)` constructor with both counts checked at compile time |
| N18 | Tensor indexing and assignment | done — v0.8.0; each subscript drops a dimension from the type, bounds-checked; a slice taken as a value is a copy, an assignment target navigates without copying; whole-row assignment `t[0] = row` |
| N19 | Tensor arithmetic `+ - * /`, unary `-` | done — v0.8.0; element-wise, same shape required, either side may be a `float`; `==` rejected with a diagnostic that names the operators that work |
| N20 | `len(t)` and `shape(t)` | done — v0.8.0; `len` is the outermost dimension, `shape` returns `[int]` so `len(shape(t))` is the rank |
| N21 | Linear algebra: `matmul`, `dot` | done — v0.8.0; `matmul` is rank 2 × (rank 2 or rank 1) so a matrix–vector layer is a single call; `dot` for two vectors |
| N22 | Activations: `relu`, `sigmoid`, `tanh`, `gelu`, `softmax` | done — v0.8.0; `softmax` works per row and subtracts the row maximum first, so logits far beyond `exp()`'s range stay finite |
| N23 | Reductions: `sum`, `mean`, `max`, `min`, `argmax` | done — v0.8.0 |
| N24 | Losses: `mse`, `cross_entropy` | done — v0.8.0; cross entropy is `m + log(Σeˣʲ−ᵐ) − xᵢ` (logsumexp), overflow-proof |
| N25 | Own `exp` / `log` in the runtime (no libm) | done — v0.8.0; argument reduction with a split `ln2`, 20-term Taylor / 18-term odd series; measured against libm over the whole range: 2.2e-16 relative for `exp`, 3.6e-16 for `log`, with `nan`/`inf`/signed zero passed through |
| N26 | Seeded uniform generator: `rand([d, …])`, `seed(n)` | done — v0.8.0; splitmix64, reproducible across runs and across machines |
| N27 | Tensor parameters and return types | done — v0.8.0; `fn scale(t: tensor[2, 3], k: float) -> tensor[2, 3]` |
| N28 | Tensor runtime is **emitted only when used** | done — v0.8.0; an AST walk sets the flag, so a program that never mentions tensors links none of it (15 KB vs 21 KB binary) |
| N29 | Builtin names stay user-definable | done — v0.8.0; only `serve`/`len`/`str`/`input_line`/`float`/`int`/`push`/`scan_*`/`tensor` are reserved. A user `sum`, `rand`, `max` or `min` shadows the builtin of the same name, so programs written before v0.8.0 keep compiling unchanged |
| N30 | `serve()` prints arrays and structs | done — v0.8.0; arrays as `[1, 2, 3]`, structs as `Name {field: value, ...}` with fields in declaration order. Nesting composes and never emits a line break of its own, so a struct holding an array, a tensor and another struct prints on one line. No run-time type tag: the compiler writes a descriptor per struct type it prints (name, field count, name/tag/extra/extra2 per field) and the walker reads it. Every address in a descriptor is stored as a **gap** (`label - descriptor`) rather than an absolute pointer, so the table carries no relocation — a program that links with `cc` (the `extern fn` path, which asks for a PIE) would otherwise get text relocations in `.rodata`. `void` remains the only thing `serve` rejects. This is what makes `serve(shape(t))` possible |

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

**Update v0.8.0 — the AI core (N17–N30):** the `tensor` type with
compile-time shapes, indexing and assignment, element-wise arithmetic,
`matmul` / `dot`, the activations, the reductions and the two losses, backed
by an own `exp`/`log` and a seeded generator — all emitted only for programs
that use tensors, so pure programs stay freestanding and small. `serve()`
gained arrays and structs (N30), which is what makes `serve(shape(t))`
possible. Builtin names
that programs have always been free to define (`sum`, `rand`, `max`, `min`,
`shape`, …) remain definable: a user definition shadows the builtin. The
suite is now **108 tests (29 runtime + 79 error), all passing.**

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

---

## Milestone M5 — The network layer: from tensors to a language for AI

v0.8.0 shipped the **numerical core**: a tensor type whose shape lives in the
type, the linear-algebra and activation primitives, and the two losses — all
computed by code this repository emits, with no libm, no libc and no runtime
to install. The next milestone turns that core into something that can
actually learn and actually talk to a model.

The guiding constraint is unchanged: **pure Ducky programs stay
freestanding.** Everything here is implemented either in generated x86-64 or
in Ducky itself, so a program that uses it still produces a static ELF that
runs on a bare machine.

### M5a — Learning

| # | Requirement | Notes |
|---|---|---|
| Y1 | Reverse-mode automatic differentiation over tensors | gradients as tensors, tape or symbolic; `grad(f, x)` or an explicit `backward()` |
| Y2 | Optimizers: SGD (with momentum), Adam | apply a gradient in place, learning rate as an argument |
| Y3 | A `Layer`/`Module` vocabulary: dense layers, chains, forward pass | structs plus functions, no new syntax if it can be avoided |
| Y4 | Training loop on a small dataset, with a printed loss curve | the proof that Y1–Y3 work end to end |
| Y5 | Deterministic data: `rand` reshaping helpers, shuffling, train/test split | builds on `seed(n)` |
| Y6 | Batched tensors (`tensor[b, n]`) and a `matmul` that stays cache-friendly | current `matmul` is a simple triple loop |

### M5b — Model files

| # | Requirement | Notes |
|---|---|---|
| Z1 | File I/O builtins: `open` / `read` / `write` / `close` / `exists` | raw syscalls for pure programs; needed to load weights at all |
| Z2 | Reading binary data into a tensor (`tensor_from_bytes`, endianness stated) | the layout must be documented, not guessed |
| Z3 | Saving and loading a trained model in a Ducky-defined format | header + shapes + float32 payloads |
| Z4 | Quantization: `float32` ↔ `int8`/`float16` weights and a quantized `matmul` | smaller files, faster inference on narrow vectors |
| Z5 | `half` / `bfloat16` as types, or as an explicit view over `float` | decide by need, not by symmetry |

### M5c — Talking to a model

| # | Requirement | Notes |
|---|---|---|
| W1 | Sockets over raw syscalls: `tcp_connect`, `read`, `write` | pure-Ducky networking, no libc |
| W2 | An HTTP/1.1 client: headers, `Content-Length`, chunked responses | enough for a JSON API |
| W3 | JSON: a parser (and a small writer) in Ducky | structured output from an API and structured input to one |
| W4 | A tokenizer: byte-pair encoding with a vocabulary file | the bridge between `string` and `tensor` |
| W5 | Transformer blocks in Ducky: embeddings, layer norm, multi-head attention, the residual stream | the same primitives v0.8.0 shipped, composed |
| W6 | A demo that runs a real prompt through a hosted LLM endpoint and prints the reply | the headline of the release |
| W7 | An offline demo: a small character-level model trained locally (Y4) and sampled (Z3) | proves the whole path works without a network |

**Why this order.** M5a needs no new I/O and can be validated purely against
numbers, so it comes first. M5b is the bridge — a model that cannot be loaded
is a model that cannot be used. M5c is the visible payoff, and it is the only
part that assumes a network at all; W7 exists so that the milestone is
complete even with the network unplugged.

**Deliberately out of scope:** GPU/NEON backends (the target is a
general-purpose CPU with SSE2), training anything larger than a laptop can
hold, and a serving stack. Those follow, if at all, in a later milestone.
