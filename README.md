# Duck (duck-lang)

A small, statically typed programming language that **compiles to native
x86-64 machine code**. The compiler (`duckc`) is written in C11, emits AT&T
assembly, and hands it to the system assembler (`as`) and linker (`ld`). The
generated executables are freestanding ELF binaries: no libc, no VM, no
interpreter — they talk to the kernel with raw syscalls.

```duck
// examples/hello.duck
fn main() -> int {
    serve("Hello, Duck!");
    return 0;
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

v0.1.0 — the initial milestone is complete: full front end, type checker,
native code generator, runtime, test suite and documentation. See
[ROADMAP.md](ROADMAP.md) for the requirements this release covers and what
comes next.

## Features

* **Static typing** with three types: `int` (64-bit), `bool`, `string`.
  No implicit conversions; every mismatch is a compile error with a caret
  diagnostic.
* **Functions** with any number of parameters (more than six use the stack,
  as the System V ABI prescribes), recursion and mutual recursion.
* **Control flow**: `if` / `else if` / `else`, `while`, short-circuit
  `&&` / `||`.
* **Block scoping** with shadowing, `let` declarations with type inference or
  explicit annotations.
* **String literals** with escapes and value equality (`==` compares contents).
* **Freestanding output**: `_start` + syscalls, so binaries run without any
  runtime installed.

## Installation

### One-liner

```sh
curl -fsSL https://raw.githubusercontent.com/didacg/duck-lang/main/install.sh | sh
```

The script clones the repository into a temporary directory, builds `duckc`
and installs it. Options:

```sh
sh install.sh --user         # install into ~/.local/bin (no root needed)
sh install.sh --prefix DIR   # install into DIR/bin
sh install.sh --uninstall    # remove it again
```

### From a checkout

```sh
git clone https://github.com/didacg/duck-lang.git
cd duck-lang
make
sudo make install            # -> /usr/local/bin/duckc
# or: ./install.sh --user
```

The installer needs `make`, a C compiler, binutils (`as`, `ld`) and `git` for
the one-liner. Linux x86-64 only.

## Building

Requirements: a C11 compiler, GNU make, GNU `as` and `ld` (binutils), Linux
x86-64.

```sh
make          # builds ./duckc
make test     # runs the test suite (22 tests)
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
   decimal/hex integers, strings with escapes, comments, operators.
2. **Parser** (`src/parser.c`) — recursive descent producing an AST in a bump
   arena, with precedence levels exactly as specified in
   [SPEC.md](SPEC.md#61-operators-and-precedence).
3. **Semantic analysis** (`src/sema.c`) — two passes: collect every function
   signature (so call order does not matter), then resolve every name, check
   every type, assign stack slots and compute frame sizes. Also performs
   definite-return analysis.
4. **Code generator** (`src/codegen.c`) — emits AT&T x86-64. Expressions use
   a push-machine; the compiler tracks stack depth at compile time so `%rsp`
   is always 16-byte aligned at each `call`, and stack arguments land exactly
   where the ABI expects them.
5. **Runtime** — emitted into every program: `_start`, `serve` support for
   ints/bools/strings and string comparison, all implemented with the
   `write` and `exit` syscalls.

## Project layout

```
duck-lang/
├── Makefile           build, test, examples and install targets
├── install.sh         standalone installer (curl | sh friendly)
├── README.md          this file
├── SPEC.md            language specification v0.1.0
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
├── examples/          hello, fibonacci, fizzbuzz
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
