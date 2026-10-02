# Duck (duck-lang)

![Duck logo: a solid pastel-red D](editors/vscode/assets/icon.svg)

A small, statically typed programming language that **compiles to native
x86-64 machine code**. The compiler (`duckc`) is written in C11, emits AT&T
assembly, and hands it to the system assembler (`as`) and linker (`ld`). The
generated executables are freestanding ELF binaries: no libc, no VM, no
interpreter — they talk to the kernel with raw syscalls.

```duck
// examples/hello.duck
fn main() -> int {
    serve("Hello, Duck!");
    send 0;
}
```

```sh
$ make
$ ./duckc examples/hello.duck -o hello
$ ./hello
Hello, Duck!
```

```sh
$ file hello
hello: ELF 64-bit LSB executable, x86-64 ...
```

---

## Status

v0.5.0 — adds **structs**: `struct Point { x: int, y: int }` declarations
with positional construction (`Point(3, 4)`), field reads and writes (`p.x`,
`p.x = 10`), reference semantics between variables, structs as parameters,
return values and fields, and declaration order that does not matter. On top
of v0.4.0's floats, arrays, string indexing and `const`; v0.3.0 introduced the
English keywords (`fn`, `let`, `if`, `else`, `send`, `serve`); the Duck-era
words `wing`, `nest`, `when`, `otherwise` (and the older `return`, `print`)
are ordinary identifiers now and are rejected with a hint when used as syntax.
See [ROADMAP.md](ROADMAP.md) for the requirements this release covers and what
comes next.

## Features

* **Static typing** with the built-in types `int` (64-bit), `float` (f64),
  `bool`, `string` and fixed-length arrays (`[int]`, `[float]`, `[bool]`,
  `[string]`), plus user-defined `struct` records. No implicit conversions;
  every mismatch is a compile error with a caret diagnostic. `float(x)` /
  `int(x)` convert explicitly.
* **Structs**: `struct Point { x: int, y: int }` with positional construction
  (`Point(3, 4)`), field access and assignment (`p.x`, `p.x = 10`), reference
  semantics (assignments share the value, like arrays), structs as parameters,
  return values and fields, and free declaration order.
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
  `brk` — still no libc.

## Installation

### One-liner

```sh
curl -fsSL https://raw.githubusercontent.com/didacg/duck-lang/main/install.sh | sh
```

The script clones the repository into a temporary directory, builds `duckc`
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
git clone https://github.com/didacg/duck-lang.git
cd duck-lang
make
sudo make install            # -> /usr/local/bin/duckc + editor extension
# or: ./install.sh --user
```

Under `sudo` the extension is still installed into the *invoking* user's
editor directory, not root's. `sudo make uninstall` removes both.

The installer needs `make`, a C compiler, binutils (`as`, `ld`) and `git` for
the one-liner. Linux x86-64 only.

### Updating

```sh
duck-update                # update Duck in place (installed with the compiler)
duck-update --check        # report whether a newer version exists
sh update.sh               # from a checkout: git pull + rebuild + reinstall
```

Or from anywhere, without a checkout:

```sh
curl -fsSL https://raw.githubusercontent.com/didacg/duck-lang/main/update.sh | sh
```

From a checkout the script fast-forwards your sources (`git pull
--ff-only`) and reinstalls them — your commits are never overwritten, and a
dirty tree makes it stop instead of clobbering your work. Anywhere else it
downloads the latest sources into a temporary directory. Options (`--user`,
`--prefix DIR`, `--no-editor`) are forwarded to `install.sh`, and
`make update PREFIX=...` does the same. `--check` compares the commit
recorded at install time (`$PREFIX/share/duck-lang/commit`) with
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
make          # builds ./duckc
make test     # runs the test suite (78 tests)
make examples # builds every examples/*.duck into build/
make clean
```

## Usage

```
duckc [options] <file.duck>

  -o <path>        write the output to <path>
  -S, --emit-asm   stop after writing x86-64 assembly
  --dump-tokens    show the token stream and exit
  -h, --help       show help
  --version        show version information
```

Without `-o`, the output name is derived from the input file
(`duckc hello.duck` produces `./hello`).

## How it works

```
source .duck ──▶ lexer ──▶ parser ──▶ semantic analysis ──▶ code generator
                tokens       AST      types + stack slots   x86-64 .s
                                                              │
                                            GNU as ──▶ .o ──▶ ld ──▶ executable
```

1. **Lexer** (`src/lexer.c`) — hand-written scanner: keywords, identifiers,
   decimal/hex integers, float literals (point and/or exponent, careful not to
   swallow the `..` range operator), strings with escapes, comments, operators.
2. **Parser** (`src/parser.c`) — recursive descent producing an AST in a bump
   arena, with precedence levels exactly as specified in
   [SPEC.md](SPEC.md#61-operators-and-precedence).
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
   concatenation, `str`, `input_line`, array bounds checks, `push` and a small
   `brk`-based allocator — all implemented with raw `write`, `read`, `brk` and
   `exit` syscalls.

## Project layout

```
duck-lang/
├── Makefile           build, test, examples and install targets
├── install.sh         standalone installer (curl | sh friendly)
├── update.sh          updater, installed as `duck-update` (curl | sh friendly)
├── editors/
│   └── vscode/        VS Code/VSCodium/Cursor extension (.duck highlighting)
├── README.md          this file
├── SPEC.md            language specification v0.5.0
├── ROADMAP.md         requirements: delivered and planned
├── src/
│   ├── common.{h,c}   arena allocator, file loading, diagnostics
│   ├── lexer.{h,c}    tokens and scanner
│   ├── ast.h          AST, types, program representation
│   ├── parser.{h,c}   recursive descent parser
│   ├── sema.{h,c}     name resolution, type checking, stack slots
│   ├── codegen.{h,c}  x86-64 backend + runtime emission
│   ├── version.h      version constants
│   └── main.c         duckc command line driver (as + ld invocation)
├── examples/          hello, fibonacci, fizzbuzz, averages, structs
└── tests/
    ├── run_tests.sh   test harness
    ├── cases/         programs with expected stdout (and exit status)
    └── errors/        programs that must fail with a given message
```

## Documentation

* [SPEC.md](SPEC.md) — the language: lexical structure, types, statements,
  expressions, precedence table, full grammar, reserved names, diagnostics.
* [ROADMAP.md](ROADMAP.md) — initial requirements and the plan forward.

## License

No license has been chosen yet — all rights reserved by the author.
