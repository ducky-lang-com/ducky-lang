# Duck Language Specification

**Version 0.1.0** — this document defines the syntax and semantics accepted by
`duckc`, the Duck compiler.

Duck is a small, statically typed, imperative language. It compiles straight to
x86-64 machine code (Linux, System V AMD64 ABI) with no runtime and no C
library behind it.

---

## 1. Hello, Duck

```duck
// hello.duck
fn main() -> int {
    serve("Hello, Duck!");
    return 0;
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
| `fn` | `let` | `return` |
| `if` | `else` | `while` |
| `true` | `false` | |
| `int` | `bool` | `string` |

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

There are **no implicit conversions**: `1 + true` is an error, not a `2`.

`void` values only exist as the result of calls to functions that do not return
anything (and of `serve`). A `void` value cannot be assigned, returned,
compared, passed as an argument or handed to `serve`.

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
    if n == 0 { return true; }
    return is_odd(n - 1);
}

fn is_odd(n: int) -> bool {
    if n == 0 { return false; }
    return is_even(n - 1);
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

### 5.6 `return`

```duck
return;              // only in functions with return type void
return expression;
```

A function whose return type is not `void` must return a value on **every**
execution path; the compiler checks this.

### 5.7 Blocks

`{ ... }` opens a new scope and may appear anywhere a statement may appear.

---

## 6. Expressions

### 6.1 Operators and precedence

From lowest to highest binding:

| Level | Operators | Associativity |
|---|---|---|
| 1 | `\|\|` | left |
| 2 | `&&` | left |
| 3 | `==` `!=` | left |
| 4 | `<` `<=` `>` `>=` | left |
| 5 | `+` `-` | left |
| 6 | `*` `/` `%` | left |
| 7 | unary `-` `!` | right (prefix) |
| 8 | `f(args)` `(...)` | — |

`&&` and `||` **short-circuit**: the right operand is not evaluated when the
result is already known.

### 6.2 Operator types

| Expression | Operand types | Result |
|---|---|---|
| `a + b`, `a - b`, `a * b`, `a / b`, `a % b` | `int`, `int` | `int` |
| `a < b`, `a <= b`, `a > b`, `a >= b` | `int`, `int` | `bool` |
| `a == b`, `a != b` | any two values of the *same* type (`int`, `bool`, `string`) | `bool` |
| `a && b`, `a \|\| b` | `bool`, `bool` | `bool` |
| `-a` | `int` | `int` |
| `!a` | `bool` | `bool` |

Strings are compared **by content**, not by address.

### 6.3 Numeric semantics

* Arithmetic wraps around on overflow (two's complement).
* `/` truncates toward zero: `-7 / 3 == -2`.
* `%` takes the sign of the dividend: `-7 % 3 == -1`, `7 % -3 == 1`.
* Division or remainder by zero traps the process (SIGFPE) — there is no
  checked arithmetic in 0.1.

### 6.4 Evaluation order

Operands of a binary operator are evaluated **left to right**. Arguments of a
call are evaluated **right to left** (an implementation detail of the System V
ABI code generator; do not rely on side effects across arguments).

### 6.5 Calls

```duck
add(1, 2)
```

The callee must be a function name. Arity and argument types must match
exactly. Any call whose function returns `void` is a valid statement but not a
value.

---

## 7. Built-in functions

```
serve(value)
```

Writes `value` (`int`, `bool` or `string`) to standard output followed by a
newline, using raw `write` syscalls (it is unbuffered). Returns `void`.

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
              | return-stmt
              | block
              | expr ";" ;

let-stmt     := "let" IDENT [ ":" type ] "=" expr ";" ;
assign-stmt  := IDENT "=" expr ";" ;
if-stmt      := "if" expr block [ "else" ( if-stmt | block ) ] ;
while-stmt   := "while" expr block ;
return-stmt  := "return" [ expr ] ";" ;

expr         := or-expr ;
or-expr      := and-expr { "||" and-expr } ;
and-expr     := eq-expr { "&&" eq-expr } ;
eq-expr      := rel-expr { ( "==" | "!=" ) rel-expr } ;
rel-expr     := add-expr { ( "<" | "<=" | ">" | ">=" ) add-expr } ;
add-expr     := mul-expr { ( "+" | "-" ) mul-expr } ;
mul-expr     := unary { ( "*" | "/" | "%" ) unary } ;
unary        := ( "-" | "!" ) unary | postfix ;
postfix      := primary [ "(" [ args ] ")" ] ;
args         := expr { "," expr } ;
primary      := INT | STRING | "true" | "false" | IDENT | "(" expr ")" ;
```

---

## 9. Reserved names

* All keywords in §2.4.
* The function name `serve` (it is a builtin).
* Function names starting with `_`.
* Function names starting with `duck_` (the generated runtime owns
  `duck_serve_int`, `duck_serve_bool`, `duck_serve_str`, `duck_streq`).

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
* `serve` writes directly with syscall 1;
* no libc, no interpreter, no virtual machine.

Requirements: Linux x86-64, GNU `as` and `ld` to assemble and link.
