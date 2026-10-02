# Ducky for VS Code / VSCodium / Cursor

![Ducky logo: a solid pastel-red D](assets/icon.svg)

Syntax highlighting and language support for the **Ducky** programming
language (`.duck` files).

* syntax highlighting: comments, strings and escapes, decimal/hex/float
  numbers (`1.5`, `1e-4` — the `..` range stays two integers, `...` the C
  variadic marker stays three dots), keywords
  (`fn`, `extern`, `const`, `let`, `if`, `else`, `while`, `for`, `in`,
  `send`, `break`, `continue`, `struct`, `import`), types (`int`, `float`,
  `bool`, `string`, `[int]` and friends),
  `true`/`false`, `const` names, function definitions and calls, the builtins
  (`serve`, `len`, `str`, `input_line`, `push`, `scan_int`, `scan_float`,
  `scan_int_line`, `scan_float_line`), operators (including the
  bitwise `& | ^ ~ << >>` and the range `..`);
* **file icon**: `.duck` files show the pastel-red D in the explorer and tabs
  (declared as the language icon, so it works with your current file icon
  theme);
* `//` and `/* */` commenting (`Shift+Alt+A`), auto-closing brackets and
  quotes, block indentation.

## Install

The extension is installed automatically with the language itself:

```sh
curl -fsSL https://raw.githubusercontent.com/ducky-lang-com/ducky-lang/main/install.sh | sh
# (or: sh install.sh / sudo make install from a checkout)
```

To manage only the extension, from the repository root:

```sh
make install-vscode
```

or directly:

```sh
sh editors/vscode/install.sh
```

The script copies the extension into every VS Code-compatible extensions
directory it finds (`~/.vscode`, `~/.vscode-oss` (VSCodium), `~/.cursor`,
`~/.vscode-server`). Then run **Developer: Reload Window** (or restart the
editor).

## Uninstall

```sh
sh editors/vscode/install.sh --uninstall
```

## Manual install

Copy this folder to `<extensions-dir>/ducky-lang-0.1.9`, e.g.

```sh
cp -r editors/vscode ~/.vscode-oss/extensions/ducky-lang-0.1.9
```
