# Duck Language Specification

**Version 0.3.0** — this document defines the syntax and semantics accepted by
`duckc`, the Duck compiler.

Duck is a small, statically typed, imperative language. It compiles straight to
x86-64 machine code (Linux, System V AMD64 ABI) with no runtime and no C
library behind it.

Its keywords are the English spellings `fn`, `let`, `if`, `else`, `while`,
`for`, `in`, `send`, `break` and `continue`, plus `true`/`false` and the type
names `int`, `bool` and `string`. The output builtin is `serve`. The Duck-era
words `wing`, `nest`, `when` and `otherwise` (canonical only in v0.2.0) and the
older `return` and `print` are ordinary identifiers now.

---

## 1. Hello, Duck

```duck
// hello.duck
fn main() -> int {
    serve("Hello, Duck!");
    send 0;
}
```

Compile and run:

```sh
./duckc hello.duck -o hello
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
| `fn` | `let` | `if` |
| `else` | `while` | `for` |
| `in` | `send` | `break` |
| `continue` | `true` | `false` |
| `int` | `bool` | `string` |

The retired words `wing`, `nest`, `when`, `otherwise`, `return` and `print`
are **not** keywords: they parse as ordinary identifiers, so `wingman` or a
variable named `nest` are valid names. Using them where syntax is expected is
an error that names the replacement (`'wing' is not a Duck keyword anymore -
use 'fn' instead`).

### 2.5 Integer literals

Decimal (`42`) or hexadecimal (`0x2A`). Literals must fit in a signed 64-bit
integer; there are no suffixes.

```
int-literal := decimal-digits | '0' ('x' | 'X') hex-digits
```

### 2.6 String literals

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
| `bool` | boolean | `true`, `false` |
| `string` | pointer to NUL-terminated bytes | `"duck"` |
| `void` | no value | only produced by calls; cannot be stored |

There are **no implicit conversions**: `1 + true` is an error, not a `2`, and
`5 & (3 == 1)` is an error too (a `bool` does not become an `int`).

`void` values only exist as the result of calls to functions that do not return
anything (and of `serve`). A `void` value cannot be assigned, returned,
compared, passed as an argument or handed to `serve`.

Strings are immutable values: `+` does not modify its operands, it builds a
new string.

---

## 4. Program structure

A program is a sequence of top-level function declarations. There are no
globals and no separate declarations: **the entry point is**

```duck
fn main() -> int { ... }
```

`main` must take no parameters and return `int`. Its return value becomes the
process exit status.

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

---

## 5. Statements

### 5.1 Declarations

```duck
let x = 10;          // type inferred from the initializer
let y: int = 10;     // explicit annotation (must match exactly)
```

A variable is visible from its declaration to the end of the enclosing block.
Redeclaring a name in the same scope is an error; shadowing an outer scope is
allowed.

### 5.2 Assignment

```duck
x = x + 1;
```

The variable must already exist and the value type must match exactly.
Assignment is a statement, not an expression.

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
| 12 | `f(args)` `(...)` | — |

The precedence follows C (there are no ternary or comma operators). `&&` and
`||` **short-circuit**: the right operand is not evaluated when the result is
already known.

Because `==` binds tighter than `&`, the expression `5 & 3 == 1` parses as
`5 & (3 == 1)` and is a type error; write `(5 & 3) == 1` if that is what you
mean.

### 6.2 Operator types

| Expression | Operand types | Result |
|---|---|---|
| `a - b`, `a * b`, `a / b`, `a % b` | `int`, `int` | `int` |
| `a + b` | `int`, `int` (add) or `string`, `string` (concatenation) | `int` / `string` |
| `a & b`, `a \| b`, `a ^ b`, `a << b`, `a >> b` | `int`, `int` | `int` |
| `a < b`, `a <= b`, `a > b`, `a >= b` | `int`, `int` | `bool` |
| `a == b`, `a != b` | any two values of the *same* type (`int`, `bool`, `string`) | `bool` |
| `a && b`, `a \|\| b` | `bool`, `bool` | `bool` |
| `-a`, `~a` | `int` | `int` |
| `!a` | `bool` | `bool` |

Strings are compared **by content**, not by address. Mixing operands of
different types in `+` (for example `"n = " + 42`) is an error: there is no
implicit conversion — use `str(...)`.

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

### 6.4 Evaluation order

Operands of a binary operator are evaluated **left to right**. Arguments of a
call are evaluated **right to left** (an implementation detail of the System V
ABI code generator; do not rely on side effects across arguments). The bounds
of a `for` range are evaluated once, left to right, before the loop starts.

### 6.5 Calls

```duck
add(1, 2)
```

The callee must be a function name. Arity and argument types must match
exactly. Any call whose function returns `void` is a valid statement but not a
value.

---

## 7. Built-in functions

| Call | Result | Description |
|---|---|---|
| `serve(value)` | `void` | Writes `value` (`int`, `bool` or `string`) to standard output followed by a newline, using raw `write` syscalls (unbuffered). |
| `len(s)` | `int` | Length of a string in bytes (no argument other than one `string` is accepted). |
| `str(value)` | `string` | Decimal text of an `int` (`str(-7) == "-7"`), or `"true"` / `"false"` of a `bool`. |
| `input_line()` | `string` | Reads one line from standard input and returns it without the trailing newline. At EOF returns `""`. Reads at most 4095 bytes per call; a longer line continues on the next call. |

`str` returns a fresh string on the heap (except the constant `true`/`false`,
which points at static memory), so it can be concatenated freely. Builtins
cannot be redefined as user functions.

---

## 8. Grammar (EBNF)

```ebnf
program      := func-decl { func-decl } ;

func-decl    := "fn" IDENT "(" [ param-list ] ")" [ "->" type ] block ;
param-list   := param { "," param } ;
param        := IDENT ":" type ;
type         := "int" | "bool" | "string" ;

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
assign-stmt  := IDENT "=" expr ";" ;
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
postfix      := primary [ "(" [ args ] ")" ] ;
args         := expr { "," expr } ;
primary      := INT | STRING | "true" | "false" | IDENT | "(" expr ")" ;
```

---

## 9. Reserved names

* All keywords in §2.4.
* The builtin function names `serve`, `len`, `str` and `input_line` (they
  cannot be redefined).
* Function names starting with `_`.
* Function names starting with `duck_` (the generated runtime owns
  `duck_serve_int`, `duck_serve_bool`, `duck_serve_str`, `duck_streq`,
  `duck_strlen`, `duck_alloc`, `duck_concat`, `duck_str_int`, `duck_str_bool`,
  `duck_input` and the data symbol `duck_brk`).

Local variables and parameters have no such restriction.

---

## 10. Diagnostics

Errors carry the file, line and column, the offending source line and a caret:

```
prog.duck:3:13: error: operator '+' requires 'int' operands, found 'int' and 'bool'
    serve(1 + true);
            ^
```

The compiler stops at the first error and exits with status `1`.

---

## 11. Execution model

The generated executable is a freestanding ELF binary:

* entry point `_start` calls `main` and exits with its value through the
  `exit` syscall (number 60);
* `serve` writes directly with syscall 1; `input_line` reads with syscall 0;
* no libc, no interpreter, no virtual machine.

**Memory.** Strings produced by `+`, by `str` and by `input_line` come from a
small bump allocator that grows the program break (`brk` syscall). Memory is
never freed — there is no garbage collector. If the program exhausts memory it
writes `duck: out of memory` to standard error and exits with status `127`.

Requirements: Linux x86-64, GNU `as` and `ld` to assemble and link.
