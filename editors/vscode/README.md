# Duck for VS Code / VSCodium / Cursor

![Duck logo: an amber monoline D](assets/icon.svg)

Syntax highlighting and language support for the **Duck** programming
language (`.duck` files).

* syntax highlighting: comments, strings and escapes, decimal/hex numbers,
  keywords (`wing`, `nest`, `when`, `otherwise`, `while`, `send`), types
  (`int`, `bool`, `string`), `true`/`false`, function definitions and calls,
  the `serve()` builtin, operators;
* **file icon**: `.duck` files show the amber D in the explorer and tabs
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

Copy this folder to `<extensions-dir>/duck-lang-0.1.3`, e.g.

```sh
cp -r editors/vscode ~/.vscode-oss/extensions/duck-lang-0.1.3
```
