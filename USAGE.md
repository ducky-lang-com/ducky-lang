# Using Ducky

A hands-on guide to the language. Every snippet here compiles with the
compiler in this repository; the full formal description lives in
[SPEC.md](SPEC.md), and [README.md](README.md) covers building and
installing.

---

## Contents

1. [Install and run](#1-install-and-run)
2. [Hello, Ducky](#2-hello-ducky)
3. [Types](#3-types)
4. [Values: `let` and `const`](#4-values-let-and-const)
5. [Operators](#5-operators)
6. [Control flow](#6-control-flow)
7. [Functions](#7-functions)
8. [Arrays](#8-arrays)
9. [Strings](#9-strings)
10. [Structs](#10-structs)
11. [Multiple files](#11-multiple-files)
12. [Calling C](#12-calling-c)
13. [Tensors](#13-tensors)
14. [The neural-net builtins](#14-the-neural-net-builtins)
15. [Gradients: `grad(f, x)`](#15-gradients-gradf-x)
16. [Errors and diagnostics](#16-errors-and-diagnostics)
17. [The output of a program](#17-the-output-of-a-program)

---

## 1. Install and run

```sh
# from a checkout
make
make test          # 121 tests

# or install it system-wide (and the editor extension)
curl -fsSL https://raw.githubusercontent.com/ducky-lang-com/ducky-lang/main/install.sh | sh
```

The compiler is `duckyc`. It takes a source file and produces a native
executable:

```sh
duckyc program.duck -o program      # ./program
duckyc program.duck -S -o program.s # stop at the assembly, for reading
duckyc --version
```

---

## 2. Hello, Ducky

```duck
fn main() -> int {
    serve("Hello, Ducky!");
    send 0;
}
```

```sh
$ duckyc hello.duck -o hello && ./hello
Hello, Ducky!
```

Two things are unusual and worth remembering:

* **`serve(value)`** is the output builtin. It prints the value and a
  newline. There is no `print` — using it produces
  *`'print' does not exist in Ducky - the output builtin is 'serve'`*.
* **`send expression;`** is how a function returns. `main` must be
  `fn main() -> int`, and its value becomes the process exit status.

The resulting binary needs nothing installed:

```sh
$ file hello
hello: ELF 64-bit LSB executable, x86-64, ... statically linked, not dynamic
```

---

## 3. Types

| Type | What it is |
|---|---|
| `int` | 64-bit signed integer |
| `float` | 64-bit IEEE-754 double |
| `bool` | `true` / `false` |
| `string` | immutable byte string |
| `[int]`, `[float]`, `[bool]`, `[string]` | fixed-shape arrays |
| `tensor[2, 3]` | multidimensional array of `float` **with its shape in the type** (§13) |
| *structs* | user-defined records (§10) |
| `void` | the result of a call that returns nothing |

**There are no implicit conversions.** `1 + 1.5` is a compile error, and so
is `"n = " + 42`. Convert on purpose:

```duck
serve(float(1) + 1.5);          // 2.5
serve("n = " + str(42));        // n = 42
serve(int(3.9));                // 3  (truncates toward zero)
```

Mixing `int` and `float` in arithmetic, comparison or assignment is always an
error, never a silent promotion — the same spirit that makes a wrong tensor
shape (§13) a compile error rather than a runtime surprise.

---

## 4. Values: `let` and `const`

```duck
let n = 42;                 // inferred: int
let pi = 3.14159;           // inferred: float
let total: float = 0.0;     // annotated
```

Type inference works from the initialiser. If you annotate, the initialiser
must match exactly — `let t: tensor[2] = zeros([2, 3])` is a compile error
naming both types.

**Constants** are top-level, literal-only, and can appear in any order:

```duck
const LIMIT = 10;
const GREETING = "quack";

fn main() -> int {
    for i in 0 .. LIMIT {
        serve(i);
    }
    send 0;
}
```

There are no mutable globals: state lives in `main` or on the stack.

**Scopes** are block-scoped and shadowing is allowed:

```duck
let x = 1;
{
    let x = 2;      // a new variable, the outer one is untouched
    serve(x);       // 2
}
serve(x);           // 1
```

---

## 5. Operators

C precedence, evaluated left to right (`&&` and `||` short-circuit):

```
||
&&
|
^
&
==  !=
<   <=  >  >=
<<  >>
+   -
*   /   %
```

* `+ - * /` work on `int`/`int` and `float`/`float`.
* `%` is **`int` only**.
* `+` also concatenates `string` and `string`.
* `& | ^ ~ << >>` are `int` only; `>>` is arithmetic.
* Comparisons return `bool`; `==` on strings compares **contents**.

Because `==` binds tighter than `&`, `5 & 3 == 1` parses as `5 & (3 == 1)`
and fails to type-check. Write `(5 & 3) == 1`.

---

## 6. Control flow

```duck
if n < 0 {
    serve("negative");
} else if n == 0 {
    serve("zero");
} else {
    serve("positive");
}
```

```duck
while i < 10 {
    i = i + 1;
}
```

```duck
// both bounds are int expressions, evaluated once, before the loop
for i in 0 .. 5 {
    serve(i);          // 0 1 2 3 4
}
```

`break` and `continue` leave or restart the **innermost** loop. The `for`
loop variable is `int` and is scoped to the body.

---

## 7. Functions

```duck
fn add(a: int, b: int) -> int {
    send a + b;
}

fn describe(name: string, n: int) -> string {
    send name + " has " + str(n);
}

fn noop() {              // no return type means void
    serve("nothing to send");
}

fn main() -> int {
    serve(add(2, 3));            // 5
    serve(describe("ducky", 7)); // ducky has 7
    noop();
    send 0;
}
```

* A function with a return type must `send` a value on **every** path; the
  compiler proves it.
* Any number of parameters. More than six use the stack, as the System V ABI
  prescribes.
* Recursion and mutual recursion work; declaration order is free.
* Tensor types are allowed in signatures (§13).

```duck
fn scale(t: tensor[2, 3], k: float) -> tensor[2, 3] {
    send t * k;
}
```

---

## 8. Arrays

```duck
let xs: [int] = [1, 2, 3];
serve(len(xs));        // 3
serve(xs[0]);          // 1
xs[0] = 42;            // bounds-checked
```

```duck
let ys = [1, 2];
ys = push(ys, 3);      // push returns a NEW array
serve(len(ys));        // 3
```

Arrays have **reference semantics**: assigning one array variable to another
makes them share the same block, exactly like structs.

One thing arrays cannot do: `==` rejects them (compare element by element
instead), with a diagnostic that says so.

```duck
serve([1, 2, 3]);        // [1, 2, 3]
serve(["a", "b"]);       // ["a", "b"]   quoted inside, bare at the top level
```

```duck
fn main() -> int {
    let xs = [3, 1, 2];
    let best = xs[0];
    for i in 1 .. len(xs) {
        if xs[i] > best { best = xs[i]; }
    }
    serve(best);        // 3
    send 0;
}
```

> A `tensor` is the other half of this story: it is a contiguous block of
> `float` with a compile-time shape, it *can* be printed, and it can be
> sliced, reduced and multiplied. See §13.

---

## 9. Strings

```duck
let s = "quack" + "!";
serve(len(s));        // 5
serve(s[0]);          // q     (a one-byte string)
serve(s == "quack!"); // true  (compares contents)
serve(input_line());  // one line from stdin, without the newline
```

`str()` turns `int`, `float` and `bool` into text. Floats print with 15
rounded significant digits and trailing zeros trimmed, so `serve(0.1 + 0.2)`
prints `0.3`, and values outside `[1e-15, 1e18)` switch to exponent form.

Parsing the other way:

```duck
serve(scan_int(" 42abc"));     // 42   (C atoi rules, never raises)
serve(scan_float("6.25e1"));   // 625
serve(scan_int_line());        // one line from stdin, parsed as int
serve(scan_float_line());
```

---

## 10. Structs

```duck
struct Point {
    x: int,
    y: int
}

fn main() -> int {
    let p = Point(3, 4);     // positional construction
    serve(p.x);              // 3
    p.y = 10;
    serve(p.y);              // 10

    let q = p;               // reference semantics: q and p share storage
    q.x = 99;
    serve(p.x);              // 99

    serve(p);                // Point {x: 99, y: 10}

    send 0;
}
```

* Fields are declared with `name: type`, comma-separated; a trailing comma is
  allowed.
* Declaration order is free — a struct can mention a struct defined later.
* Structs can be parameters, return values and fields of other structs.
* A struct and a function may not share a name.
* You cannot compare whole structs — compare the fields you care about.
* `serve` prints one as `Point {x: 1, y: 2}`: the struct's name, then every
  field in declaration order. Nesting works the same way (see §16).

---

## 11. Multiple files

```duck
import "lib/utils.duck";   // path relative to this file
```

* The import graph is loaded depth first and **deduplicated by canonical
  path**: diamonds and cycles load each file exactly once.
* All files share **one global namespace** — functions, structs and
  constants declared anywhere are visible everywhere, and `main` may live in
  any of them.
* Diagnostics name the file they come from.
* `import` is only valid at the top level; a missing file, a directory or a
  non-string path is a compile error.

---

## 12. Calling C

Pure Ducky programs are freestanding: `_start`, raw `write`/`read`/`brk`/
`exit` syscalls, no libc at all. Declaring a C function opts in explicitly:

```duck
extern fn printf(fmt: string, ...) -> int;
extern fn abs(n: int) -> int;
extern fn sqrt(x: float) -> float;

fn main() -> int {
    printf("abs(-17) = %d\n", abs(-17));
    printf("sqrt(2) = %.13f\n", sqrt(2.0));
    send 0;
}
```

* Signature types are limited to `int`, `float`, `bool`, `string` (and no
  return type for `void`).
* `...` makes it variadic; there is a minimum-arity check.
* Arguments are marshalled per System V AMD64: integers and pointers to
  `rdi…r9`, floats to `xmm0…xmm7`, the rest on the stack, with `%al` set for
  variadics. Float returns come back in `xmm0`.
* The driver links with `cc -nostartfiles -lm` **only** when a file declares
  `extern`, so the default build stays freestanding.

`extern fn main` is rejected, and so are arrays and structs in an extern
signature.

---

## 13. Tensors

This is what makes Ducky interesting for machine learning. A tensor is an
array of `float` whose **shape is part of its type**.

```duck
let w: tensor[2, 3] = tensor([2, 3], [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]);
let x: tensor[3]    = tensor([3], [1.0, 0.0, -1.0]);

serve(w);        // [[1, 2, 3], [4, 5, 6]]
serve(x);        // [1, 0, -1]
```

### Dimensions are compile-time constants

Each dimension must be a **literal or a top-level `const`**:

```duck
const K = 3;
fn main() -> int {
    let a: tensor[2, K] = zeros([2, K]);   // fine: K is a top-level const
    serve(a);
    send 0;
}
```

```duck
let n = 3;                          // a local variable is not a constant
let b: tensor[n] = zeros([n]);
```

They must be positive, at most `1000000000`, and the rank at most `8`. Because
the shape is static, the element count is known while compiling — so the
constructor's counts, every subscript and every shape mismatch are checked
before the program ever runs.

### The constructor

```text
tensor(shape, data)
```

`shape` is an `[int]` of dimensions, `data` a `[float]` with exactly
`product(shape)` elements:

```duck
let t: tensor[2, 3] = tensor([2, 3], [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]);
let bad: tensor[2, 3] = tensor([2, 3], [1.0, 2.0, 3.0]);  // wrong length
```

Both mismatches are compile errors that quote the two counts.

### Reading the shape

```duck
serve(len(w));           // 2          -- the outermost dimension
serve(shape(w));         // [2, 3]     -- the whole shape
serve(shape(w)[1]);      // 3
serve(len(shape(w)));    // 2          -- the rank
```

### Indexing and assignment

Each subscript **drops one dimension from the type**:

```duck
serve(w[0]);       // [1, 2, 3]      tensor[3]
serve(w[1][2]);    // 6              float
```

A slice taken as a value is a **copy**:

```duck
let row: tensor[3] = w[0];
row[0] = 9.0;
serve(w);          // unchanged
```

but an assignment target navigates **without** copying, so these write into
`w`:

```duck
w[1][0] = 99.0;          // one element
w[1] = other_row;        // a whole row
```

Every subscript is bounds-checked at run time.

### Arithmetic

```duck
let a: tensor[3] = tensor([3], [1.0, 2.0, 4.0]);
let b: tensor[3] = tensor([3], [8.0, 4.0, 2.0]);

serve(a + b);        // [9, 6, 6]
serve(a - b);        // [-7, -2, 2]
serve(a * b);        // [8, 8, 8]
serve(a / b);        // [0.125, 0.5, 2]
serve(a * 2.0);      // [2, 4, 8]     a float may be on either side
serve(1.0 - a);      // [0, -1, -3]
serve(-a);           // [-1, -2, -4]
```

Both operands of a tensor operator must have the **same shape**; otherwise:

```
operator '+' requires tensors of the same shape, found 'tensor[2, 3]' and 'tensor[3, 2]'
```

There is no `==` for tensors. Element-wise comparison would need a boolean
tensor that does not exist, and comparing shapes is not what a value means
here — the diagnostic names the operators that do work.

### Linear algebra

```duck
let m: tensor[2, 3] = tensor([2, 3], [1.0, 0.0, -1.0, 2.0, 3.0, -2.0]);
let n: tensor[3, 2] = tensor([3, 2], [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]);
let v: tensor[3]    = tensor([3], [1.0, 0.0, -1.0]);

serve(matmul(m, n));      // [[-4, -4], [1, 4]]        tensor[2, 2]
serve(matmul(n, m));      // [[5, 6, -5], [11, 12, -11], [17, 18, -17]]
serve(matmul(m, v));      // [2, 4]                    tensor[2]  matrix x vector
serve(dot(v, v));         // 2
```

`matmul` takes a rank-2 tensor on the left and a rank-2 **or rank-1** tensor
on the right — a rank-1 right operand is a single column, so a layer is one
call. The inner dimensions must agree:

```
matmul cannot multiply 'tensor[2, 3]' by 'tensor[2, 3]': the inner dimensions differ (3 and 2)
```

### Tensors in signatures

```duck
fn scale(t: tensor[2, 3], k: float) -> tensor[2, 3] {
    send t * k;
}

fn main() -> int {
    serve(scale(m, 2.0));
    send 0;
}
```

The shape travels in the type, so a function states exactly what it accepts
and callers are checked against it.

### Storage

A tensor is laid out exactly like an array of `float`: a 64-bit element count
followed by the elements in row-major order — one contiguous block of
`8 * (1 + count)` bytes. Rank, dimensions and strides exist **only in the
type**; nothing is stored at run time. The tensor runtime itself is emitted
only for programs that use tensors, so a program that never mentions them
links none of it and stays freestanding.

---

## 14. The neural-net builtins

All of these operate on tensors and are checked against the shapes in their
types.

| Call | Returns | Notes |
|---|---|---|
| `tensor(shape, data)` | tensor | constructor |
| `zeros([d, …])`, `ones([d, …])` | tensor | filled with `0.0` / `1.0` |
| `rand([d, …])` | tensor | uniform in `[0, 1)` |
| `seed(n)` | `void` | same `n` ⇒ same sequence, every run |
| `shape(t)` | `[int]` | all dimensions |
| `len(t)` | `int` | outermost dimension |
| `matmul(a, b)` | tensor | rank 2 × (rank 2 or rank 1) |
| `dot(a, b)` | `float` | two rank-1 tensors, same length |
| `relu(x)` | tensor | `max(0, x)` |
| `sigmoid(x)` | tensor | `1 / (1 + e^-x)` |
| `tanh(x)` | tensor | `tanh x` |
| `gelu(x)` | tensor | the tanh approximation |
| `softmax(x)` | tensor | per row; each row sums to `1` |
| `sum(x)`, `mean(x)` | `float` | over all elements |
| `max(x)`, `min(x)` | `float` | over all elements |
| `argmax(x)` | `int` | flat index of the largest (first, on a tie) |
| `mse(p, y)` | `float` | mean squared error, same shapes |
| `cross_entropy(logits, i)` | `float` | `logsumexp(logits) - logits[i]` |
| `step(x)` | tensor | `1` where `x > 0`, else `0` — the derivative of `relu` |
| `matmul_tn(a, b)` | tensor | `aᵀ · b` |
| `matmul_nt(a, b)` | tensor | `a · bᵀ` |
| `cross_entropy_grad(logits, i)` | tensor | `softmax(logits) - onehot(i)` |

The last four are the pieces the backward pass of [`grad`](#15-gradients-gradf-x)
is written with; they are ordinary calls with their shapes checked like any
other, so a hand-written training loop can use them directly.

### A forward pass

```duck
fn main() -> int {
    let x: tensor[3] = tensor([3], [1.0, 0.0, -1.0]);
    let w: tensor[2, 3] = tensor([2, 3], [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]);
    let bias: tensor[2] = tensor([2], [0.1, -0.2]);

    let h = matmul(w, x) + bias;   // tensor[2]
    let a = relu(h);               // tensor[2]
    serve(a);

    // a two-class head over a vector of logits
    let logits: tensor[3] = tensor([3], [2.0, 1.0, 0.1]);
    serve(softmax(logits));        // sums to 1
    serve(cross_entropy(logits, 0));
    send 0;
}
```

### Softmax does not overflow

`softmax` subtracts the row maximum before exponentiating, so inputs far
beyond `exp()`'s range still come out as a distribution:

```duck
serve(softmax(tensor([3], [1000.0, 1000.0, 1000.0])));
// [0.333333333333333, 0.333333333333333, 0.333333333333333]
```

`cross_entropy` uses the same logsumexp identity, so it is overflow-proof
too. The compiler emits its own `exp` and `log` (measured at ~1 ulp against
libm over the whole range), which is why none of this needs `libm`.

### Reproducible randomness

```duck
seed(42);
serve(rand([4]));
seed(42);
serve(rand([4]));   // identical to the line above
```

The generator is splitmix64 with an explicit state, so a seeded run produces
the same numbers on every machine.

### Working with rows

`softmax` and the arithmetic helpers work element-wise or per row; `sum`,
`mean`, `max`, `min` and `argmax` reduce over **all** elements, and indexing
is how you reach one row:

```duck
let t: tensor[2, 3] = tensor([2, 3], [1.0, 2.0, 3.0, 1.0, 0.0, -1.0]);
serve(sum(t));            // 6
serve(mean(t));           // 1
serve(t[0]);              // [1, 2, 3]
serve(argmax(t));         // 2
```

---

## 15. Gradients: `grad(f, x)`

`grad(f, x)` is the derivative of `f` with respect to `x`. It is a
compile-time special form: the compiler writes a second function, `f$grad`,
next to `f` and replaces the call with an ordinary call to it, so what runs
is plain Ducky with no runtime tape and no bookkeeping.

```duck
fn loss(w: tensor[2, 3]) -> float {
    let s = sum(w);
    send s * s;                     // 2*sum(w) for every element
}

fn main() -> int {
    let w: tensor[2, 3] = tensor([2, 3], [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]);
    serve(grad(loss, w));           // [[42, 42, 42], [42, 42, 42]]
    send 0;
}
```

The result has the same type as `x`. For that to work, `f` must:

* take exactly **one** parameter — a `float`, a tensor, or a struct whose
  fields are floats and tensors;
* return **`float`**, the quantity being minimised;
* be a straight line: only `let` statements and a final `send`. No `if`, no
  loops, no assignment, no `break`, no calls to other functions.

Anything else is a compile error that names the offending statement, so the
limit shows up where you wrote the code rather than as a wrong number later.

A struct parameter comes back as a struct with a gradient for each of its
leaves, which is what makes the update step shape-checked at compile time:

```duck
struct Params {
    w: tensor[2, 2],
    b: float,
}

fn step_loss(p: Params) -> float {
    let h = matmul(p.w, tensor([2], [1.0, 1.0]));
    let a = relu(h);
    send mean(a) + p.b * 0.01;
}

fn main() -> int {
    let p = Params(tensor([2, 2], [1.0, 2.0, 3.0, 4.0]), 0.5);
    let g = grad(step_loss, p);
    serve(g);                       // Params {w: [[0.5, 0.5], [0.5, 0.5]], b: 0.01}
    send 0;
}
```

What the chain rule covers today: `+ - * /`, unary `-`, `matmul`, `dot`,
`relu`, `sigmoid`, `tanh`, `gelu`, `sum`, `mean`, `mse`, `cross_entropy`,
tensor indexing and struct fields. Constants contribute nothing — `zeros`,
`ones`, `rand`, `tensor(...)` and literals are simply skipped, so a fixed
input or a fixed target needs no special treatment, and neither does a value
with no gradient in it (`shape(t)`, `argmax(t)`, `int`, `bool`). A call that
*could* carry a gradient and has no rule yet — `softmax`, `max`, `min` — is
rejected with a message naming it rather than silently returning zero.

Two `grad(f, …)` sites in the same program share one `f$grad`. Defining `f`
in terms of `grad(f, …)` is rejected rather than unrolled.

---

## 16. Errors and diagnostics

Errors carry the file, line and column, the offending source line and a caret:

```
prog.duck:4:11: error: operator '+' requires tensors of the same shape, found 'tensor[2, 3]' and 'tensor[3, 2]'
    serve(a + b);
            ^
```

Nothing is written when compilation fails. A few classes worth knowing:

| Situation | Message |
|---|---|
| no `fn main() -> int` | `the program must define an entry point: 'fn main() -> int'` |
| a missing `send` on some path | definite-return analysis |
| `1 + 1.5` | `operator '+' requires two 'float' values or two 'int' values - Ducky has no implicit conversion (use float(x)), found 'int' and 'float'` |
| `serve(nothing())` on a `void` function | `cannot pass a value of type 'void' to serve()` |
| `print("x")` | `'print' does not exist in Ducky - the output builtin is 'serve'` |
| a wrong tensor shape | `type mismatch: 't' is declared as 'tensor[2]' but the initializer has type 'tensor[2, 3]'` |
| a non-constant dimension | `tensor dimension 'n' must be a compile-time integer constant` |
| `grad(f, x)` on a body with an `if` | `grad() cannot differentiate 'f': only 'let' statements and a final 'send' are supported (found 'if')` |
| `grad(f, x)` calling another function | `grad() cannot differentiate 'f': the call to 'g()' is not a builtin` |
| index past the end | run time: `ducky: index out of bounds`, exit `127` |

**Redefinition.** A handful of names are reserved and cannot be declared:
`serve`, `len`, `str`, `input_line`, `float`, `int`, `push`, the `scan_*`
family and `tensor`, plus — since differentiation — `grad`, `step`,
`matmul_tn`, `matmul_nt` and `cross_entropy_grad`: the last four are real
builtins, but the gradient code the compiler emits names them, so a program
defining its own would silently change what `grad` produces. Everything else
in the builtin surface is an ordinary name — a program may define its own
`sum`, `rand`, `max`, `min` or `shape`, and **the user's definition wins** at
every call site. That is what keeps programs written before v0.8.0 compiling
unchanged.

---

## 17. The output of a program

`serve` writes through raw `write` syscalls, unbuffered, one value per line:

```duck
struct Point { x: int, y: int }

fn main() -> int {
    serve(42);            // 42
    serve(4.5);           // 4.5
    serve(true);          // true
    serve("hi");          // hi
    serve([1, 2, 3]);     // [1, 2, 3]
    serve(["a", "b"]);    // ["a", "b"]
    serve(Point(3, -4));  // Point {x: 3, y: -4}
    serve(tensor([3], [1.0, 2.0, 3.0]));   // [1, 2, 3]
    serve(tensor([2, 3], [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]));
    send 0;
}
```

The last one prints `[[1, 2, 3], [4, 5, 6]]`.

Composites nest, and nesting never emits a line break of its own — so a
struct holding an array, a tensor and another struct comes out on one line:

```duck
struct Point { x: int, y: int }
struct Account {
    owner: string,
    balance: float,
    active: bool,
    tags: [string],
    home: Point
}

fn main() -> int {
    serve(Account("ducky", 12.5, true, ["vip"], Point(1, 2)));
    // Account {owner: "ducky", balance: 12.5, active: true,
    //          tags: ["vip"], home: Point {x: 1, y: 2}}
    send 0;
}
```

Two formatting rules are worth remembering:

* **Strings are bare at the top level and quoted inside a container.**
  `serve("hi")` prints `hi`, `serve(["hi"])` prints `["hi"]`.
* **Floats print with 15 rounded significant digits and trailing zeros
  trimmed**, so `0.1 + 0.2` prints `0.3` and `[1.5, 2.0]` prints `[1.5, 2]`.

The process exit status is `main`'s return value.

---

## Where to go next

* [SPEC.md](SPEC.md) — the formal grammar, the full builtin table, the
  precedence table and every diagnostic class.
* [ROADMAP.md](ROADMAP.md) — what has landed and what comes next:
  optimizers, model files, sockets and HTTP, and the transformer blocks on
  top of the primitives here.
* [examples/tensors.duck](examples/tensors.duck) — everything in §13 and §14
  in one runnable file.
* [examples/grad.duck](examples/grad.duck) — every shape of §15 in one
  runnable file.
