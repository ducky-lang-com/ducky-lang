# Duck for VS Code / VSCodium / Cursor

![Duck logo: a solid pastel-red D](assets/icon.svg)

Syntax highlighting and language support for the **Duck** programming
language (`.duck` files).

* syntax highlighting: comments, strings and escapes, decimal/hex/float
  numbers (`1.5`, `1e-4` — the `..` range stays two integers), keywords
  (`fn`, `const`, `let`, `if`, `else`, `while`, `for`, `in`, `send`, `break`,
  `continue`), types (`int`, `float`, `bool`, `string`, `[int]` and friends),
  `true`/`false`, `const` names, function definitions and calls, the builtins
  (`serve`, `len`, `str`, `input_line`, `push`), operators (including the
  bitwise `& | ^ ~ << >>` and the range `..`);
* **file icon**: `.duck` files show the pastel-red D in the explorer and tabs
  (declared as the language icon, so it works with your current file icon
  theme);
* `//` and `/* */` commenting (`Shift+Alt+A`), auto-closing brackets and
  quotes, block indentation.

## Install

The extension is installed automatically with the language itself:

```sh
curl -fsSL https://raw.githubusercontent.com/didacg/duck-lang/main/install.sh | sh
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

Copy this folder to `<extensions-dir>/duck-lang-0.1.6`, e.g.

```sh
cp -r editors/vscode ~/.vscode-oss/extensions/duck-lang-0.1.6
```
