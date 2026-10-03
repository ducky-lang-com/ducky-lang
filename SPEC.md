# Ducky Language Specification

**Version 0.8.0** — this document defines the syntax and semantics accepted by
`duckyc`, the Ducky compiler.

Ducky is a small, statically typed, imperative language. It compiles straight to
x86-64 machine code (Linux, System V AMD64 ABI). Programs built from Ducky
features alone are freestanding: no runtime, no C library behind them.
`extern` declarations (§4.3) opt into the C library instead.

Its keywords are the English spellings `fn`, `extern`, `struct`, `import`,
`const`, `let`, `if`, `else`, `while`, `for`, `in`, `send`, `break` and
`continue`, plus `true`/`false`, the type names `int`, `float`, `bool`,
`string` and `tensor` (§3.3). The output builtin is `serve`. The retired words
`wing`, `nest`,
`when` and `otherwise` (canonical only in v0.2.0) and the older `return` and
`print` are ordinary identifiers now.

---

## 1. Hello, Ducky

```duck
// hello.duck
fn main() -> int {
    serve("Hello, Ducky!");
    send 0;
}
```

Compile and run:

```sh
./duckyc hello.duck -o hello
./hello
```

---

## 2. Lexical structure

### 2.1 Source text

Source files are UTF-8 (ASCII is enough for everything the language defines so
far) and use the `.duck` extension.

### 2.2 Whitespace and comments

Spaces, tabs, carriage returns and newlines separate tokens. Two comment forms
exist:

```duck
// a line comment runs to the end of the line
/* a block comment
   may span several lines */
```

Block comments do not nest.

### 2.3 Identifiers

```
identifier := (letter | '_') { letter | digit | '_' }
letter     := 'A'..'Z' | 'a'..'z'
digit      := '0'..'9'
```

Identifiers are case sensitive.

### 2.4 Keywords

| | | |
|---|---|---|
| `fn` | `extern` | `struct` |
| `import` | `const` | `let` |
| `if` | `else` | `while` |
| `for` | `in` | `send` |
| `break` | `continue` | `true` |
| `false` | `int` | `float` |
| `bool` | `string` | `tensor` |

The retired words `wing`, `nest`, `when`, `otherwise`, `return` and `print`
are **not** keywords: they parse as ordinary identifiers, so `wingman` or a
variable named `nest` are valid names. Using them where syntax is expected is
an error that names the replacement (`'wing' is not a Ducky keyword anymore -
use 'fn' instead`).

### 2.5 Integer literals

Decimal (`42`) or hexadecimal (`0x2A`). Literals must fit in a signed 64-bit
integer; there are no suffixes.

```
int-literal := decimal-digits | '0' ('x' | 'X') hex-digits
```

### 2.6 Float literals

A decimal point (`1.5`, `0.001`) and/or an exponent (`1e10`, `2.5e-4`). A `.`
directly followed by another `.` is the `for` range operator, so `0 .. 5` and
`0..5` both lex as two integers and a `..`; a float needs at least one digit
before the point (`.5` is not a literal). Float literals are parsed at compile
time with the C `strtod` rules (binary64).

```
float-literal := decimal-digits '.' decimal-digits [ exponent ]
               | decimal-digits exponent
exponent      := ('e' | 'E') [ '+' | '-' ] decimal-digits
```

### 2.7 String literals

Double quoted, may not span lines. Escape sequences: `\n`, `\t`, `\r`,
`\\`, `\"`. A string cannot contain a NUL byte.

```
string-literal := '"' { char | escape } '"'
escape         := '\n' | '\t' | '\r' | '\\' | '\"'
```

---

## 3. Types

| Type | Representation | Literals |
|---|---|---|
| `int` | 64-bit signed two's complement | `0`, `-7`, `0xFF` |
| `float` | 64-bit IEEE-754 binary64 | `1.5`, `0.25`, `1e10` |
| `bool` | boolean | `true`, `false` |
| `string` | pointer to NUL-terminated bytes | `"duck"` |
| `[int]`, `[float]`, `[bool]`, `[string]` | pointer to a heap block `[count][elems…]` | `[1, 2, 3]`, `[]` (with annotation) |
| *struct* (`Point`, …) | pointer to a heap block of 8-byte field slots | `Point(1, 2)` (constructor) |
| `tensor[…]` (§3.3) | pointer to a heap block `[count][elems…]`, identical to an array of `float` | `tensor([2, 3], [1.0, …])` (constructor) |
| `void` | no value | only produced by calls; cannot be stored |

There are **no implicit conversions**: `1 + true` is an error, not a `2`, and
`1 + 1.5` is an error too (an `int` does not become a `float`). Convert
explicitly with `float(x)` (int → float) or `int(x)` (float → int,
truncating toward zero).

`void` values only exist as the result of calls to functions that do not return
anything (and of `serve`). A `void` value cannot be assigned, returned,
compared, passed as an argument or handed to `serve`.

Strings are immutable values: `+` does not modify its operands, it builds a
new string, and no expression can write into one (`s[0] = "x"` is an error).

### 3.1 Arrays

Arrays have a **fixed element type** (one of the four above) and a **fixed
length** decided by the literal that creates them:

```duck
let xs = [1, 2, 3];        // [int], length 3
let fs: [float] = [];      // empty, only with an annotation
```

* **Length** comes from `len(array)`; it never changes.
* **Elements** are read with `xs[i]` and written with `xs[i] = v`. Every
  access is bounds-checked at run time: an index outside `0 .. len-1`
  (including negative indexes) prints `ducky: index out of bounds` to standard
  error and exits with status `127`.
* **Arrays are references.** `let ys = xs` makes `ys` point at the same block,
  so `xs[0] = 7` is visible through `ys`. `push(xs, v)` is the exception: it
  copies into a **new** array and returns it (`xs = push(xs, v)`), leaving the
  old one untouched for everyone still holding it.
* **Growing**: `push(array, value) -> array` returns a new array with the value
  appended. Together with an annotated empty literal this builds arrays of
  unknown length in a loop.
* **Nested arrays** (`[[int]]`) are not supported yet.
* Arrays cannot be compared with `==` — compare elements instead.
* Elements live in 8-byte slots: `int`, `float`, `bool` and `string` values
  (pointers) are stored as-is.

`const` declarations may also be of any of the scalar types (see §4).

### 3.2 Structs

A `struct` declaration introduces a **nominal** record type:

```duck
struct Point {
    x: int,
    y: int,
}
```

* **Fields** are `name: Type` pairs separated by commas (a trailing comma is
  allowed). Every field type is allowed — including other structs and arrays —
  but a struct may not contain itself through a *value*: fields are references,
  so `struct Node { next: Node }` parses and only fails if you try to build an
  infinitely deep value (there is no `null` yet).
* **Declaration order is free**: a struct may be used before the line that
  declares it, exactly like functions and constants.
* **Construction** is positional and complete: `Point(1, 2)` must pass exactly
  one argument per field, each with the field's type.
* **Field access** reads with `p.x` and writes with `p.x = v`. Field names are
  ordinary identifiers (not keywords), looked up on the static type of the
  receiver: an unknown field or a receiver that is not a struct is a compile
  error.
* **Structs are references.** `let q = p` makes `q` point at the same block, so
  `q.x = 7` is visible through `p` (like arrays). There is no copy operation.
* **Comparison is rejected**: `p == q` and `p != q` are errors — compare the
  fields you care about.
* **`serve` cannot print a struct** (there is no record syntax yet); pass the
  fields one by one.
* **Arrays of structs** (`[Point]`) are not supported yet, like nested arrays.
* Values of struct type may be parameters, `let` initializers (with a
  `Point` annotation) and return values; they travel as plain pointers.

`Point` used as a value (without parentheses) is an error that names the fix:
`'Point' is a struct type - construct a value with Point(...)`.

---

### 3.3 Tensors

A tensor is a multidimensional array of `float` whose **shape is part of its
type**: `tensor[2, 3]` and `tensor[3, 2]` are different types, and mixing them
is a compile error rather than a run-time surprise.

```duck
let w: tensor[2, 3] = tensor([2, 3], [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]);
let x: tensor[3]    = tensor([3], [1.0, 0.0, -1.0]);
```

**Dimensions are compile-time integers.** Each one must be a literal or a
top-level `const NAME = <int>;` — a variable is rejected (`tensor dimension
'n' must be a compile-time integer constant`). Every dimension must be
positive and at most `1000000000`, and the rank (the number of dimensions) at
most `8`. Because the shape is static, the element count is known at compile
time, every index can be checked, and no shape bookkeeping is stored at run
time.

**Representation.** A tensor is laid out exactly like an array of `float`: a
64-bit element count followed by the elements in row-major order. It is one
contiguous block of `8 * (1 + count)` bytes. Everything else — the rank, the
dimensions, the strides — exists only in the type.

**Constructor.** `tensor(shape, data)` takes an `[int]` of dimensions and a
`[float]` with exactly `product(dims)` elements. Both counts are checked at
compile time: `tensor()` holds 6 elements but the data literal has 4 is a
compile error, as is a data literal that is not a `[float]`.

**`len` and `shape`.** `len(t)` is the **outermost** dimension (`len(w)` is
`2`). `shape(t)` returns an `[int]` with every dimension, so `len(shape(t))`
is the rank and `shape(t)[i]` is dimension `i`.

**Indexing.** Each subscript drops one dimension from the type:
`w[0]` is `tensor[3]`, `w[0][2]` is `float`. The index must be an `int` and
is bounds-checked at run time. A slice taken into a variable **is a copy**:

```duck
let row: tensor[3] = w[0];   // a copy, like an array
row[0] = 9.0;                // w is untouched
```

but an assignment target navigates **without** copying, so writing through it
updates the original: `w[0][2] = 9.0;` and `w[1] = other_row;` both write into
`w`.

**Operators.** `+ - * /` are element-wise and require tensors of the **same
shape** (`operator '+' requires tensors of the same shape, found 'tensor[2, 3]'
and 'tensor[3, 2]'`). Either side may instead be a `float`, in which case it is
applied to every element (`2.0 * x`, `1.0 - x`). Unary `-` negates. There is
no `==` / `!=` for tensors — comparing shapes would be meaningless at this
level, and an element-wise comparison would need a boolean tensor that does
not exist; the diagnostic says so and lists the operators that do work.

**Linear algebra and the neural-net primitives** are builtins rather than
operators: `matmul`, `dot`, the activations (`relu`, `sigmoid`, `tanh`,
`gelu`, `softmax`), the reductions (`sum`, `mean`, `max`, `min`, `argmax`)
and the losses (`mse`, `cross_entropy`). §7 lists them all; [USAGE.md](USAGE.md)
shows them in context.

**Function signatures carry shapes.** A parameter and a return type can be a
tensor type, so a function states exactly what it accepts:

```duck
fn scale(t: tensor[2, 3], k: float) -> tensor[2, 3] {
    send t * k;
}
```

---

## 4. Program structure

A program starts from the single file given to `duckyc`. That file is a
sequence of top-level declarations: **functions** (`fn`), **extern
declarations** (`extern`, §4.3), **structs** (`struct`), **constants**
(`const`) and **imports** (`import`, §4.2). There
are no mutable globals: **the entry point is**

```duck
fn main() -> int { ... }
```

`main` must take no parameters and return `int`. Its return value becomes the
process exit status.

### 4.1 Constants

```duck
const LIMIT = 10;
const NAME = "duck";
const OK = true;
const RATE = 0.25;
```

* The initializer must be a **single literal** (`int`, `float`, `bool` or
  `string`) — no expressions and no other constants in it (v0.4.0).
* The name must be a plain identifier: it may not start with `_` or `ducky_`,
  may not be a builtin, and may not collide with a function, a struct or
  another constant.
* Declaration order does not matter: a constant may be used before its
  declaration in the file.
* A constant occupies no storage — every reference is replaced by its literal
  during compilation. A local `let` may shadow a constant inside its scope.

Functions may be used before they are declared, so mutual recursion works:

```duck
fn is_even(n: int) -> bool {
    if n == 0 { send true; }
    send is_odd(n - 1);
}

fn is_odd(n: int) -> bool {
    if n == 0 { send false; }
    send is_even(n - 1);
}
```

Omitting `-> type` gives the function return type `void`.

### 4.2 Source files and `import`

```duck
import "lib/utils.duck";
import "../shared/types.duck";
```

* An `import` statement may appear **only at the top level** and takes a
  string literal containing the complete file name, extension included
  (v0.6.0). `import` inside a function body is an error.
* The path is resolved **relative to the directory of the file that contains
  the statement**; an absolute path is used as written.
* Imported files may import further files. The whole graph is loaded depth
  first and each file is loaded **at most once**, no matter how many times it
  is reached (diamond) or whether the imports form a cycle.
* Every file of the program shares **one global namespace**: functions,
  structs and constants declared anywhere are visible everywhere, declaration
  order does not matter across files, and duplicate names in different files
  are rejected exactly like duplicates inside a single file.
* `main` may live in any file of the program.
* Each file is parsed with its own token stream, so errors always name the
  file, line and column they occur in - a type error in an imported file is
  reported against that file, not against the root.

### 4.3 Extern declarations and the C library

```duck
extern fn printf(fmt: string, ...) -> int;
extern fn abs(n: int) -> int;
extern fn sqrt(x: float) -> float;
extern fn srand(seed: int);
```

* An `extern fn` **declares** a C function; it has no body and ends with `;`
  (v0.7.0). The compiler never generates a definition: the program is linked
  with the C library and the symbol resolves at link time.
* Parameter and return types are limited to `int`, `float`, `bool` and
  `string`, plus no return type at all (`void`). Arrays and structs have no
  C ABI mapping and are rejected.
* A trailing `...` marks the C **variadic** form (`printf`-style) and must
  come last. Calls must pass **at least** the fixed parameters; every extra
  argument must be `int`, `float`, `bool` or `string`.
* At the call site the System V AMD64 ABI is honored: `int`/`bool`/`string`
  arguments go into `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9` in argument order
  and `float` arguments into `xmm0`…`xmm7` in argument order; an argument
  whose class has exhausted its registers goes on the stack, again in
  argument order. `%al` carries the number of vector registers used, which is
  what variadic functions such as `printf` read to find floating-point
  arguments. A `float` result comes back in `xmm0`.
* A Ducky `string` argument becomes a `char *`; a `string` result is taken as
  a `char *` and must be NUL-terminated. Ducky `int` is 64-bit (`long` on
  Linux); a C function that returns a plain 32-bit `int` still works in
  practice, but formally only the low half is meaningful.
* `extern` declarations live at the top level, may appear in any file of the
  program (§4.2 shares them through the global namespace) and follow the same
  duplicate-name and reserved-name rules as `fn`.
* `main` may **not** be declared `extern`: the entry point must be defined in
  Ducky.
* The driver links a program that declares `extern` with
  `cc -nostartfiles -lm`, so the executable becomes a dynamically linked ELF
  that needs the C library at run time. Programs without `extern` keep the
  raw `ld` link and stay freestanding (§11).
* Output ordering caveat: `serve` writes immediately through a raw `write`
  syscall, while `printf` goes through the C library's stdout buffer — which
  is flushed only when it fills or at program exit (a program with `extern`
  flushes it before exiting). When stdout is not a terminal, all `printf`
  output therefore appears after all `serve` output. Prefer one output style
  per program.

---

## 5. Statements

### 5.1 Declarations

```duck
let x = 10;              // type inferred from the initializer
let y: int = 10;         // explicit annotation (must match exactly)
let xs = [1, 2, 3];      // [int] inferred from the elements
let empty: [float] = []; // an empty literal needs an annotation
```

A variable is visible from its declaration to the end of the enclosing block.
Redeclaring a name in the same scope is an error; shadowing an outer scope is
allowed (including shadowing a `const`).

### 5.2 Assignment

```duck
x = x + 1;
xs[0] = 42;          // element of an array
p.x = 42;            // field of a struct
```

The variable must already exist and the value type must match exactly.
Assignment is a statement, not an expression. An element assignment requires
an **array** on the left (`s[0] = "x"` is an error: strings are immutable);
the index must be an `int` and is bounds-checked at run time. A field
assignment requires a **struct** on the left and the field's exact type on the
right; the receiver is evaluated once.

### 5.3 Expression statement

```duck
serve(x);            // typically a call
```

### 5.4 `if` / `else if` / `else`

```duck
if score >= 90 {
    serve("A");
} else if score >= 50 {
    serve("B");
} else {
    serve("C");
}
```

The condition must have type `bool`.

### 5.5 `while`

```duck
while i < 10 {
    i = i + 1;
}
```

The condition must have type `bool`.

### 5.6 `for` (ranges)

```duck
for i in 1 .. 5 {
    serve(i);        // 1 2 3 4 — the end bound is exclusive
}
```

* Both bounds are `int` expressions and are evaluated **once**, before the
  loop starts.
* The loop variable is `int` and is scoped to the body: it does not exist
  after the loop, and it cannot be redeclared inside the body.
* The range `a .. b` runs while the variable is `< b`; an empty range
  (`5 .. 5`, `9 .. 2`) executes no iterations.
* Assigning to the loop variable inside the body changes the iteration.

### 5.7 `break` and `continue`

```duck
while true {
    break;           // leaves the innermost loop
}

for i in 0 .. 10 {
    if i % 2 == 0 { continue; }   // jumps to the next iteration
}
```

Both may only appear inside a loop (`while` or `for`), at any nesting depth
(including inside `if`s and blocks within the loop). `break` and `continue`
always refer to the **innermost** enclosing loop. In a `for` loop,
`continue` jumps to the step (the loop variable is incremented), not to the
condition test.

### 5.8 `send`

```duck
send;              // only in functions with return type void
send expression;
```

A function whose return type is not `void` must return a value on **every**
execution path; the compiler checks this.

### 5.9 Blocks

`{ ... }` opens a new scope and may appear anywhere a statement may appear.

---

## 6. Expressions

### 6.1 Operators and precedence

From lowest to highest binding:

| Level | Operators | Associativity |
|---|---|---|
| 1 | `\|\|` | left |
| 2 | `&&` | left |
| 3 | `\|` | left |
| 4 | `^` | left |
| 5 | `&` | left |
| 6 | `==` `!=` | left |
| 7 | `<` `<=` `>` `>=` | left |
| 8 | `<<` `>>` | left |
| 9 | `+` `-` | left |
| 10 | `*` `/` `%` | left |
| 11 | unary `-` `!` `~` | right (prefix) |
| 12 | `f(args)` `(...)` `a[i]` `a.b` | — |

The precedence follows C (there are no ternary or comma operators). `&&` and
`||` **short-circuit**: the right operand is not evaluated when the result is
already known.

Because `==` binds tighter than `&`, the expression `5 & 3 == 1` parses as
`5 & (3 == 1)` and is a type error; write `(5 & 3) == 1` if that is what you
mean.

### 6.2 Operator types

| Expression | Operand types | Result |
|---|---|---|
| `a - b`, `a * b`, `a / b` | `int`,`int` or `float`,`float` | same type |
| `a % b` | `int`, `int` | `int` |
| `a + b` | `int`,`int` (add), `float`,`float` (add) or `string`,`string` (concat) | same type |
| `a & b`, `a \| b`, `a ^ b`, `a << b`, `a >> b` | `int`, `int` | `int` |
| `a < b`, `a <= b`, `a > b`, `a >= b` | `int`,`int` or `float`,`float` | `bool` |
| `a == b`, `a != b` | any two values of the *same* type (`int`, `float`, `bool`, `string`) | `bool` |
| `a + b`, `a - b`, `a * b`, `a / b` on tensors | both the **same** tensor type, or a tensor and a `float` (either side) | tensor of that shape |
| `-a` on a tensor | tensor | same type |
| `a && b`, `a \|\| b` | `bool`, `bool` | `bool` |
| `-a` | `int` or `float` | same type |
| `~a` | `int` | `int` |
| `!a` | `bool` | `bool` |
| `a[i]` | `i` is `int`; `a` is an array (element type), a `string` (`string`) or a tensor (shape minus the first dimension) | element / tail type |
| `a.b` | `a` is a value of a struct type with field `b` | field type |
| `a == b` on arrays | — | **error** (compare elements) |
| `a == b` on structs | — | **error** (compare fields) |
| `a == b` on tensors | — | **error** (§3.3 names the operators that work) |

Strings are compared **by content**, not by address. Mixing operands of
different types in `+` (for example `"n = " + 42`) is an error: there is no
implicit conversion — use `str(...)` for `int`/`float`/`bool` and
`float(x)` / `int(x)` between the numeric types. Mixing `int` and `float`
anywhere (arithmetic, comparison) is likewise an error.

### 6.3 Numeric semantics

* Arithmetic wraps around on overflow (two's complement).
* `/` truncates toward zero: `-7 / 3 == -2`.
* `%` takes the sign of the dividend: `-7 % 3 == -1`, `7 % -3 == 1`.
* Division or remainder by zero traps the process (SIGFPE) — there is no
  checked arithmetic yet.
* Bitwise operators (`&`, `|`, `^`, `~`) follow the two's-complement bit
  pattern.
* `>>` is **arithmetic**: it extends the sign (`-16 >> 2 == -4`). The shift
  count is used modulo 64 (x86-64 semantics), so shifting by 64 or more wraps.

Floats are IEEE-754 binary64 with the hardware semantics:

* Division follows IEEE-754: `1.0 / 0.0` is `inf`, `-1.0 / 0.0` is `-inf`
  and `0.0 / 0.0` is `nan` (integers still trap with SIGFPE on `/0`).
* Comparisons are IEEE-754: everything involving `nan` is false **except**
  `!=`, which is true.
* `serve`/`str` print **15 significant digits, rounded** (not truncated) with
  trailing zeros trimmed: `0.1 + 0.2` prints `0.3`, `1.0 / 3.0` prints
  `0.333333333333333`, `2.675` prints `2.675`. Outside
  `[1e-15, 1e18)` the exponent form is used: `1e20` prints `1e+20`,
  `1e-20` prints `1e-20`. Special values print as `inf`, `-inf`, `nan`;
  both zeros print `0`.
* `int(x)` truncates toward zero (`int(-2.99) == -2`). Converting a `float`
  outside the `int` range yields the platform's integer-indeterminate value.

### 6.4 Evaluation order

Operands of a binary operator are evaluated **left to right**. Arguments of a
call are evaluated **right to left** (an implementation detail of the System V
ABI code generator; do not rely on side effects across arguments). The bounds
of a `for` range are evaluated once, left to right, before the loop starts.

### 6.5 Calls

```duck
add(1, 2)
Point(1, 2)         // struct constructor
```

The callee must be a function name or a **struct name**. A struct call is a
constructor: it must pass exactly one argument per field, each with the
field's type, and it evaluates to a fresh value (§3.2). Any other call's
arity and argument types must match exactly. A call whose function returns
`void` is a valid statement but not a value.

---

## 7. Built-in functions

| Call | Result | Description |
|---|---|---|
| `serve(value)` | `void` | Writes `value` (`int`, `float`, `bool` or `string`) to standard output followed by a newline, using raw `write` syscalls (unbuffered). Arrays, structs and `void` are rejected. |
| `len(x)` | `int` | Length of a string in bytes, the number of elements of an array, or the **outermost dimension** of a tensor (§3.3). |
| `str(value)` | `string` | Decimal text of an `int` (`str(-7) == "-7"`), the float form of a `float` (§6.3), or `"true"` / `"false"` of a `bool`. |
| `input_line()` | `string` | Reads one line from standard input and returns it without the trailing newline. At EOF returns `""`. Reads at most 4095 bytes per call; a longer line continues on the next call. |
| `float(x)` | `float` | Converts an `int` to `float` (exact for values up to 2^53). |
| `int(x)` | `int` | Converts a `float` to `int`, truncating toward zero. |
| `push(a, v)` | array | Returns a **new** array with `v` appended to `a`; `a` must be an array and `v` its element type. |
| `scan_int(s)` | `int` | Parses `s` like C's `atoi` (v0.7.0): skips leading whitespace, accepts an optional `+`/`-`, then decimal digits stopping at the first non-digit (hex is **not** recognized: `scan_int("0x10")` is `0`). No digits gives `0`; a value too large for `int` clamps to ±9223372036854775807. |
| `scan_float(s)` | `float` | Parses `s` like C's `atof` (v0.7.0): whitespace, optional sign, then a decimal number with an optional `.` and `e`/`E` exponent, keeping the first 15 significant digits. No digits gives `0.0`; trailing junk is ignored; `inf` and `nan` are accepted (case-insensitively, as prefixes); overflow yields ±`inf`, underflow `0.0`. |
| `scan_int_line()` | `int` | Equivalent to `scan_int(input_line())`: reads one line from standard input and parses it. |
| `scan_float_line()` | `float` | Equivalent to `scan_float(input_line())`: reads one line from standard input and parses it. |

#### Tensor builtins (v0.8.0)

| Call | Result | Description |
|---|---|---|
| `tensor(shape, data)` | tensor | Builds a tensor of the given shape from an `[int]` of dimensions and a `[float]` literal with exactly `product(shape)` elements. Both counts are checked at compile time. |
| `zeros([d, …])`, `ones([d, …])` | tensor | A tensor of the given shape filled with `0.0` / `1.0`. The shape is an `[int]` expression whose length is known at compile time. |
| `rand([d, …])` | tensor | Uniform values in `[0.0, 1.0)`, from the seeded generator. |
| `seed(n)` | `void` | Reseed the generator. The same `n` always reproduces the same sequence, across runs. |
| `shape(t)` | `[int]` | Every dimension of `t`. |
| `matmul(a, b)` | tensor | `a` is rank 2 and `b` is rank 2 or rank 1 (a single column); the inner dimensions must agree. Rank 2 × rank 1 returns `tensor[a₀]`. |
| `dot(a, b)` | `float` | Dot product of two rank-1 tensors of the same length. |
| `relu(x)`, `sigmoid(x)`, `tanh(x)`, `gelu(x)` | tensor | Element-wise activations: `max(0, x)`, `1/(1+e⁻ˣ)`, `(e²ˣ−1)/(e²ˣ+1)` and the tanh approximation `0.5x(1+tanh(√(2/π)(x+0.044715x³)))`. |
| `softmax(x)` | tensor | Per row: `eˣⁱ−ᵐ / Σeˣʲ−ᵐ`, where `m` is the row maximum. Subtracting `m` keeps large logits finite, so each row sums to `1`. |
| `sum(x)`, `mean(x)` | `float` | Total / average of all elements. |
| `max(x)`, `min(x)` | `float` | Largest / smallest element. |
| `argmax(x)` | `int` | Flat index of the largest element (the first one, on a tie). |
| `mse(p, y)` | `float` | Mean squared error between tensors of the same shape: `Σ(pᵢ−yᵢ)²/n`. |
| `cross_entropy(logits, i)` | `float` | `logsumexp(logits) − logits[i]`, computed as `m + log(Σeˣʲ−ᵐ) − xᵢ` so large logits never overflow. |

`str` returns a fresh string on the heap (except the constant `true`/`false`,
which points at static memory), so it can be concatenated freely.

The names in §9 — `serve`, `len`, `str`, `input_line`, `float`, `int`, `push`,
the `scan_*` family and `tensor` — are reserved and cannot be redefined. Every
other builtin, including all the tensor builtins above, is a plain name that
**user code may shadow**: a function declared as `sum` or `rand` takes
precedence over the builtin of the same name at every call site, which is what
lets programs written before v0.8.0 keep compiling.

---

## 8. Grammar (EBNF)

```ebnf
program      := { import-decl | func-decl | extern-decl | struct-decl | const-decl } ;

import-decl  := "import" STRING ";" ;
func-decl    := "fn" IDENT "(" [ param-list ] ")" [ "->" type ] block ;
extern-decl  := "extern" "fn" IDENT "(" [ extern-params ] ")" [ "->" type ] ";" ;
extern-params := "..." | param { "," param } [ "," "..." ] ;
param-list   := param { "," param } ;
param        := IDENT ":" type ;
struct-decl  := "struct" IDENT "{" [ field-list ] "}" ;
field-list   := field { "," field } [ "," ] ;   (* trailing comma allowed *)
field        := IDENT ":" type ;
const-decl   := "const" IDENT "=" literal ";" ;
type         := "int" | "float" | "bool" | "string"
              | "[" type "]"                (* one level: [int] etc. *)
              | "tensor" "[" dim-list "]"   (* dim-list: INT { "," INT } *)
              | IDENT ;                     (* a struct name *)

literal      := INT | FLOAT | STRING | "true" | "false" ;

block        := "{" { stmt } "}" ;

stmt         := let-stmt
              | assign-stmt
              | if-stmt
              | while-stmt
              | for-stmt
              | break-stmt
              | continue-stmt
              | return-stmt
              | block
              | expr ";" ;

let-stmt     := "let" IDENT [ ":" type ] "=" expr ";" ;
assign-stmt  := IDENT "=" expr ";"
              | expr "=" expr ";" ;  (* target must end in a[i] or a.b *)
if-stmt      := "if" expr block [ "else" ( if-stmt | block ) ] ;
while-stmt   := "while" expr block ;
for-stmt     := "for" IDENT "in" expr ".." expr block ;
break-stmt   := "break" ";" ;
continue-stmt:= "continue" ";" ;
return-stmt  := "send" [ expr ] ";" ;

expr         := or-expr ;
or-expr      := and-expr { "||" and-expr } ;
and-expr     := bitor-expr { "&&" bitor-expr } ;
bitor-expr   := bitxor-expr { "|" bitxor-expr } ;
bitxor-expr  := bitand-expr { "^" bitand-expr } ;
bitand-expr  := eq-expr { "&" eq-expr } ;
eq-expr      := rel-expr { ( "==" | "!=" ) rel-expr } ;
rel-expr     := shift-expr { ( "<" | "<=" | ">" | ">=" ) shift-expr } ;
shift-expr   := add-expr { ( "<<" | ">>" ) add-expr } ;
add-expr     := mul-expr { ( "+" | "-" ) mul-expr } ;
mul-expr     := unary { ( "*" | "/" | "%" ) unary } ;
unary        := ( "-" | "!" | "~" ) unary | postfix ;
postfix      := primary { "(" [ args ] ")" | "[" expr "]" | "." IDENT } ;
args         := expr { "," expr } ;
primary      := INT | FLOAT | STRING | "true" | "false" | IDENT
              | array-literal | tensor-ctor | "(" expr ")" ;
array-literal := "[" [ expr { "," expr } ] "]" ;
tensor-ctor  := "tensor" "(" expr "," expr ")" ;  (* shape, flat data *)
```

---

## 9. Reserved names

* All keywords in §2.4.
* The reserved builtin names `serve`, `len`, `str`, `input_line`, `float`,
  `int`, `push`, `scan_int`, `scan_float`, `scan_int_line`,
  `scan_float_line` and `tensor` (they cannot be redefined). Every other
  builtin — including the whole tensor family of §7 — is shadowable by a user
  definition of the same name.
* Function, constant, struct and field names starting with `_`.
* Function, constant, struct and field names starting with `ducky_` (the
  generated runtime owns `ducky_serve_int`, `ducky_serve_bool`, `ducky_serve_str`,
  `ducky_serve_float`, `ducky_streq`, `ducky_strlen`, `ducky_alloc`, `ducky_concat`,
  `ducky_str_int`, `ducky_str_bool`, `ducky_str_float`, `ducky_fmt_float`,
  `ducky_str_at`, `ducky_push`, `ducky_oob`, `ducky_input`, `ducky_scan_int`,
  `ducky_scan_float` and the data symbol `ducky_brk`). The pre-rename `duck_`
  prefix is free again as of v0.7.0. From v0.8.0 the tensor runtime adds
  `ducky_exp`, `ducky_log`, `ducky_dot`, `ducky_matmul`, `ducky_softmax`,
  `ducky_serve_tensor`, `ducky_trand`, `ducky_rngstate` and the rest of that
  family; they are emitted only for programs that use tensors (§3.3).
* Function and struct names may not collide with each other or with a
  constant; field names live in their own namespace (accessed through `.`).

Local variables and parameters have no such restriction.

---

## 10. Diagnostics

Errors carry the file, line and column, the offending source line and a caret:

```
prog.duck:3:13: error: operator '+' requires 'int' operands, found 'int' and 'bool'
    serve(1 + true);
            ^
```

With `import` (§4.2) a program spans several files; the reported file is
always the file the error occurs in.

The compiler stops at the first error and exits with status `1`.

---

## 11. Execution model

A program that declares no `extern` functions (§4.3) becomes a freestanding
ELF binary:

* entry point `_start` calls `main` and exits with its value through the
  `exit` syscall (number 60);
* `serve` writes directly with syscall 1; `input_line` reads with syscall 0;
* no libc, no interpreter, no virtual machine.

A program that declares `extern` functions links through
`cc -nostartfiles -lm`, so the executable is a dynamically linked ELF that
needs the C library at run time. The generated Ducky code is the same either
way: only the link step differs, plus `_start` flushing stdio
(`fflush(NULL)`) before the exit syscall so `printf` output is not lost.

**Memory.** Strings produced by `+`, by `str` and by `input_line`, and all
array blocks, come from a small bump allocator that grows the program break
(`brk` syscall). Memory is never freed — there is no garbage collector. If
the program exhausts memory it writes `ducky: out of memory` to standard error
and exits with status `127`.

**Arrays.** An array is one heap block laid out as
`[count:int64][elem0]…[elemN-1]`; the variable holds a pointer, so copies
share the block. Every read and write checks `0 <= index < count`
(unsignedly, so negatives are caught) and exits with
`ducky: index out of bounds` and status `127` on failure. `push` allocates a
new block and copies the old contents.

Requirements: Linux x86-64, GNU `as` and `ld` to assemble and link, plus a C
toolchain (`cc`) when the program declares `extern` functions.
