# Ducky (ducky-lang)

![Ducky logo: a solid pastel-red D](editors/vscode/assets/icon.svg)

A small, statically typed programming language that **compiles to native
x86-64 machine code**. The compiler (`duckyc`) is written in C11, emits AT&T
assembly, and hands it to the system assembler (`as`) and linker (`ld`).
Programs written in pure Ducky produce freestanding ELF binaries: no libc, no
VM, no interpreter — they talk to the kernel with raw syscalls. Programs that
declare C functions with `extern fn ...;` link against the C library instead,
so calling `printf`, `abs` or `sqrt` works too.

```duck
// examples/hello.duck
fn main() -> int {
    serve("Hello, Ducky!");
    send 0;
}
```

```sh
$ make
$ ./duckyc examples/hello.duck -o hello
$ ./hello
Hello, Ducky!
```

```sh
$ file hello
hello: ELF 64-bit LSB executable, x86-64 ...
```

---

## Status

v0.8.0 — **the AI core**: a `tensor` type whose **shape lives in the type**
(`tensor[2, 3]` and `tensor[3, 2]` are different types), so a wrong shape is a
compile error with a caret instead of a surprise at run time. Element-wise
`+ - * /`, indexing that drops a dimension per subscript, `matmul` (including
matrix × vector), `dot`, the activations `relu` / `sigmoid` / `tanh` /
`gelu` / `softmax`, the reductions `sum` / `mean` / `max` / `min` /
`argmax`, and the losses `mse` / `cross_entropy`. It runs on an `exp` and a
`log` this compiler emits itself — 1 ulp against libm across the whole range —
plus a seeded generator, so pure programs still link **no libc and no libm**.
The tensor runtime is emitted only when a program actually uses tensors.
See [USAGE.md](USAGE.md) for the walkthrough and [ROADMAP.md](ROADMAP.md) for
what comes next: automatic differentiation, model files and the network layer.

**Before that, v0.7.0 — renamed to Ducky**: project, language, compiler
(`duckc` → `duckyc`), runtime prefix (`ducky_*`), diagnostics (`ducky:`),
repository (`ducky-lang-com/ducky-lang`) and extension (`ducky-lang`); the
source extension stays `.duck`. The release also adds the `scan_*` builtins
(`scan_int`, `scan_float`, `scan_int_line`, `scan_float_line` parse a string
or a line from stdin C-style and never raise at run time) and **`extern fn
...;` declarations**: `extern fn printf(fmt: string, ...) -> int;` lets a
Ducky program call any C function with correct System V ABI marshalling.
The driver switches from the raw `ld` link to `cc -nostartfiles -lm` only
when `extern` is used, so pure-Ducky programs stay freestanding.

On top of v0.6.0's **multi-file programs**: `import "lib/utils.duck";` at the
top level pulls other source files into the program. Imports resolve relative
to the importing file, follow transitively, load each file only once (diamonds
and cycles are fine) and all files share one global namespace — functions,
structs and constants are visible across files, and errors name the file they
occur in. On top of v0.5.0's structs (`struct Point { x: int, y: int }` with
positional construction `Point(3, 4)` and field access `p.x`), v0.4.0's
floats, arrays, string indexing and `const`; v0.3.0 introduced the English
keywords (`fn`, `let`, `if`, `else`, `send`, `serve`); the retired words
`wing`, `nest`, `when`, `otherwise` (and the older `return`, `print`) are
ordinary identifiers now and are rejected with a hint when used as syntax.
See [ROADMAP.md](ROADMAP.md) for the requirements this release covers and what
comes next.

## Features

* **Tensors with compile-time shapes**: `tensor[2, 3]` carries its shape in
  the type, so `a + b` on mismatched shapes, a wrong constructor length or a
  dimension that is not a compile-time constant are all compile errors.
  Element-wise `+ - * /` (tensor/tensor and tensor/scalar in both orders),
  indexing that drops one dimension per subscript, `len()` (outermost dim) and
  `shape()`; `matmul` for rank 2 × (rank 2 or rank 1) and `dot` for vectors.
  Byte-identical to a `[float]` at run time — no shape bookkeeping, one
  contiguous block, bounds-checked on every index.
* **Neural-net primitives**: `relu`, `sigmoid`, `tanh`, `gelu`, `softmax`
  (per row, max-subtracted so large logits stay finite), the reductions
  `sum` / `mean` / `max` / `min` / `argmax`, and the losses `mse` /
  `cross_entropy` (logsumexp, overflow-proof). Backed by an own `exp`/`log`
  accurate to ~1 ulp, so the whole thing still links no libm — and the tensor
  runtime is emitted only for programs that use tensors.
* **Static typing** with the built-in types `int` (64-bit), `float` (f64),
  `bool`, `string` and fixed-length arrays (`[int]`, `[float]`, `[bool]`,
  `[string]`), plus user-defined `struct` records. No implicit conversions;
  every mismatch is a compile error with a caret diagnostic. `float(x)` /
  `int(x)` convert explicitly.
* **Structs**: `struct Point { x: int, y: int }` with positional construction
  (`Point(3, 4)`), field access and assignment (`p.x`, `p.x = 10`), reference
  semantics (assignments share the value, like arrays), structs as parameters,
  return values and fields, and free declaration order.
* **Multi-file programs**: `import "lib/utils.duck";` at the top level pulls
  a source file into the program. Paths are relative to the importing file,
  imports follow transitively, every file is loaded at most once (diamonds
  and cycles are fine), and all files share one global namespace — functions,
  structs and constants can be used across files, with diagnostics that name
  the file they occur in.
* **Input parsing**: `scan_int(" 42abc")` and `scan_float("6.25")` parse a
  string C-style — whitespace, optional sign, decimal only, stop at the first
  invalid character; a failed parse returns `0` / `0.0` like C's `atoi`
  instead of raising. `scan_int_line()` / `scan_float_line()` read one line
  from stdin and parse it.
* **C interop**: `extern fn ...;` declarations call real C functions —
  `extern fn printf(fmt: string, ...) -> int;`, optionally variadic (`...`),
  with System V AMD64 argument placement (integers to `rdi…r9`, floats to
  `xmm0…xmm7`, the rest on the stack) and libc linked in by the driver.
* **Functions and variables** with `fn` / `let` (type inference or explicit
  annotation), `send` for returns, `serve` for output, and top-level `const`
  declarations (`const LIMIT = 10;` — literals only, order does not matter).
* **Control flow**: `if` / `else if` / `else`, `while`, `for i in a .. b`
  ranges, `break` and `continue`, short-circuit `&&` / `||`.
* **Operators** with C precedence: arithmetic on `int` and `float`,
  comparisons, logic, and bitwise `& | ^ ~ << >>` (arithmetic `>>`), plus
  string concatenation with `+`.
* **Arrays**: literals (`[1, 2, 3]`), bounds-checked indexing and element
  assignment (`xs[0] = 42`), `len()`, `push()` (copy-on-append builds arrays
  of unknown length), reference semantics between variables.
* **Strings**: literals with escapes, value equality (`==` compares
  contents), `len()`, indexing `s[i]` (returns a one-byte string),
  `str()` for numbers and booleans, `input_line()` for stdin.
* **Float formatting**: 15 rounded significant digits with trailing zeros
  trimmed (`0.1 + 0.2` → `0.3`), exponent form outside `[1e-15, 1e18)`, and
  `inf` / `-inf` / `nan`.
* **Functions** with any number of parameters (more than six use the stack,
  as the System V ABI prescribes), recursion and mutual recursion.
* **Block scoping** with shadowing.
* **Freestanding output**: `_start` + syscalls, so binaries run without any
  runtime installed. Strings and array blocks come from a bump allocator over
  `brk` — still no libc, unless the program declares `extern` functions, in
  which case libc is linked on purpose.

## Installation

### One-liner

```sh
curl -fsSL https://raw.githubusercontent.com/ducky-lang-com/ducky-lang/main/install.sh | sh
```

The script clones the repository into a temporary directory, builds `duckyc`
and installs it **together with the editor extension** (syntax highlighting
for `.duck` files in VS Code, VSCodium and Cursor). Options:

```sh
sh install.sh --user         # install into ~/.local/bin (no root needed)
sh install.sh --prefix DIR   # install into DIR/bin
sh install.sh --no-editor    # compiler only, skip the editor extension
sh install.sh --uninstall    # remove both again
```

### From a checkout

```sh
git clone https://github.com/ducky-lang-com/ducky-lang.git
cd ducky-lang
make
sudo make install            # -> /usr/local/bin/duckyc + editor extension
# or: ./install.sh --user
```

Under `sudo` the extension is still installed into the *invoking* user's
editor directory, not root's. `sudo make uninstall` removes both.

The installer needs `make`, a C compiler, binutils (`as`, `ld`) and `git` for
the one-liner. Linux x86-64 only.

### Updating

```sh
ducky-update                # update Ducky in place (installed with the compiler)
ducky-update --check        # report whether a newer version exists
sh update.sh               # from a checkout: git pull + rebuild + reinstall
```

Or from anywhere, without a checkout:

```sh
curl -fsSL https://raw.githubusercontent.com/ducky-lang-com/ducky-lang/main/update.sh | sh
```

From a checkout the script fast-forwards your sources (`git pull
--ff-only`) and reinstalls them — your commits are never overwritten, and a
dirty tree makes it stop instead of clobbering your work. Anywhere else it
downloads the latest sources into a temporary directory. Options (`--user`,
`--prefix DIR`, `--no-editor`) are forwarded to `install.sh`, and
`make update PREFIX=...` does the same. `--check` compares the commit
recorded at install time (`$PREFIX/share/ducky-lang/commit`) with
`origin/main` and changes nothing.

## Editors

The main installer (`install.sh`, `sudo make install`, the curl one-liner)
installs this extension along with the compiler — skip it with
`--no-editor`. To manage it on its own:

```sh
make install-vscode      # copies it into every editor found
make uninstall-vscode
```

Then run **Developer: Reload Window** (or restart the editor). It lives in
[`editors/vscode/`](editors/vscode/) and can also be installed by hand — see
its README. The extension gives `.duck` files the pastel-red D as their **file
icon** in the explorer and tabs (declared as the language icon, so it works
with your current file icon theme — no theme switch needed). It is not
published on the Marketplace / Open VSX yet.

## Building

Requirements: a C11 compiler, GNU make, GNU `as` and `ld` (binutils), Linux
x86-64.

```sh
make          # builds ./duckyc
make test     # runs the test suite (108 tests)
make examples # builds every examples/*.duck into build/
make clean
```

## Usage

```
duckyc [options] <file.duck>

  -o <path>        write the output to <path>
  -S, --emit-asm   stop after writing x86-64 assembly
  --dump-tokens    show the token stream and exit
  -h, --help       show help
  --version        show version information
```

Without `-o`, the output name is derived from the input file
(`duckyc hello.duck` produces `./hello`).

A program can span several files:

```duck
import "lib/utils.duck";   // path relative to this file

fn main() -> int {
    serve(helper());
    send 0;
}
```

## How it works

```
source .duck ──▶ lexer ──▶ parser ──▶ semantic analysis ──▶ code generator
                tokens       AST      types + stack slots   x86-64 .s
                                                              │
                                            GNU as ──▶ .o ──▶ ld ──▶ executable
                                                          └──▶ cc (extern) ──┘
```

1. **Lexer** (`src/lexer.c`) — hand-written scanner: keywords, identifiers,
   decimal/hex integers, float literals (point and/or exponent, careful not to
   swallow the `..` range or `...` variadic tokens), strings with escapes,
   comments, operators.
2. **Parser** (`src/parser.c`) — recursive descent producing an AST in a bump
   arena, with precedence levels exactly as specified in
   [SPEC.md](SPEC.md#61-operators-and-precedence). Programs are loaded file
   by file (`import`): each file is parsed with its own token stream — so
   diagnostics name that file — and the declarations of every file are merged
   into one program before analysis.
3. **Semantic analysis** (`src/sema.c`) — passes that collect `const`
   declarations and every function signature (so order does not matter), then
   resolve every name (substituting constant literals in place), check every
   type, assign stack slots and compute frame sizes. Also performs
   definite-return analysis.
4. **Code generator** (`src/codegen.c`) — emits AT&T x86-64. Expressions use
   a push-machine; the compiler tracks stack depth at compile time so `%rsp`
   is always 16-byte aligned at each `call`, and stack arguments land exactly
   where the ABI expects them. Floats travel through `%rax` as their bit
   pattern and only enter the XMM registers for each operation.
5. **Runtime** — emitted into every program: `_start`, `serve` support for
   ints/floats/bools/strings, float formatting, string comparison,
   concatenation, `str`, `input_line`, the `scan_int` / `scan_float` parsers,
   array bounds checks, `push` and a small `brk`-based allocator — all
   implemented with raw `write`, `read`, `brk` and `exit` syscalls. Programs
   with `extern` declarations additionally flush stdio before exiting.
   Programs that use tensors additionally get the **tensor runtime** — the
   element-wise kernels, `matmul`, `dot`, the activations and reductions,
   the losses, the tensor printer, a seeded generator and an own `exp`/`log` —
   emitted only when the program actually uses tensors.

## Project layout

```
ducky-lang/
├── Makefile           build, test, examples and install targets
├── install.sh         standalone installer (curl | sh friendly)
├── update.sh          updater, installed as `ducky-update` (curl | sh friendly)
├── editors/
│   └── vscode/        VS Code/VSCodium/Cursor extension (.duck highlighting)
├── README.md          this file
├── USAGE.md           day-to-day usage guide with examples
├── SPEC.md            language specification v0.8.0
├── ROADMAP.md         requirements: delivered and planned
├── src/
│   ├── common.{h,c}   arena allocator, file loading, diagnostics
│   ├── lexer.{h,c}    tokens and scanner
│   ├── ast.h          AST, types, program representation
│   ├── parser.{h,c}   recursive descent parser
│   ├── sema.{h,c}     name resolution, type checking, stack slots
│   ├── codegen.{h,c}  x86-64 backend + runtime emission
│   ├── version.h      version constants
│   └── main.c         duckyc command line driver (as + ld/cc invocation)
├── examples/          hello, fibonacci, fizzbuzz, averages, structs, imports,
│                      scan, tensors
└── tests/
    ├── run_tests.sh   test harness
    ├── cases/         programs with expected stdout (and exit status)
    └── errors/        programs that must fail with a given message
```

## Documentation

* [USAGE.md](USAGE.md) — how to use the language day to day: install,
  compile, the tour of every feature with runnable snippets, and a
  walkthrough of the tensor / neural-net builtins.
* [SPEC.md](SPEC.md) — the language: lexical structure, types (including
  §3.3 Tensors), statements, expressions, precedence table, the full builtin
  table, full grammar, reserved names, diagnostics.
* [ROADMAP.md](ROADMAP.md) — requirements per milestone and the plan forward
  (v0.8.0's tensor core, then automatic differentiation, model files and the
  network layer).

## License

No license has been chosen yet — all rights reserved by the author.
