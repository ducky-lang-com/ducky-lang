# Makefile - build the Ducky language compiler (duckyc).

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
LDFLAGS ?=

SRC := src/common.c src/lexer.c src/parser.c src/sema.c src/codegen.c src/main.c
OBJ := $(SRC:.c=.o)
BIN := duckyc

.PHONY: all clean test examples install uninstall install-vscode uninstall-vscode update

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(BIN)
	@bash tests/run_tests.sh

PREFIX ?= /usr/local
BINDIR := $(DESTDIR)$(PREFIX)/bin

# The compiler, the updater and the editor extension travel together:
# by default this also installs syntax highlighting for .duck files.
install: $(BIN)
	install -d $(BINDIR)
	install -m 755 $(BIN) $(BINDIR)/$(BIN)
	@echo "installed $(BINDIR)/$(BIN)"
	@if [ -f update.sh ]; then \
		install -m 755 update.sh $(BINDIR)/ducky-update; \
		echo "installed $(BINDIR)/ducky-update"; \
	fi
	@mkdir -p $(DESTDIR)$(PREFIX)/share/ducky-lang
	@git rev-parse HEAD > $(DESTDIR)$(PREFIX)/share/ducky-lang/commit 2>/dev/null \
		|| echo unknown > $(DESTDIR)$(PREFIX)/share/ducky-lang/commit
	@sh editors/vscode/install.sh --optional

uninstall:
	rm -f $(BINDIR)/$(BIN) $(BINDIR)/ducky-update
	@echo "removed $(BINDIR)/$(BIN) and $(BINDIR)/ducky-update (if present)"
	rm -rf $(DESTDIR)$(PREFIX)/share/ducky-lang
	@sh editors/vscode/install.sh --uninstall --optional

# Update Ducky from origin/main and reinstall (forwards PREFIX and friends).
update:
	@sh update.sh --prefix $(PREFIX)

# Syntax highlighting for .duck files in VS Code / VSCodium / Cursor.
install-vscode:
	@sh editors/vscode/install.sh

uninstall-vscode:
	@sh editors/vscode/install.sh --uninstall

examples: $(BIN)
	@mkdir -p build
	@for f in examples/*.duck; do \
		echo "  duckyc $$f"; \
		./$(BIN) "$$f" -o "build/$$(basename $$f .duck)" || exit 1; \
	done

clean:
	rm -f $(OBJ) $(BIN)
	rm -rf build
