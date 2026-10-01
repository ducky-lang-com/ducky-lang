# Makefile - build the Duck language compiler (duckc).

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
LDFLAGS ?=

SRC := src/common.c src/lexer.c src/parser.c src/sema.c src/codegen.c src/main.c
OBJ := $(SRC:.c=.o)
BIN := duckc

.PHONY: all clean test examples install uninstall install-vscode uninstall-vscode

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(BIN)
	@bash tests/run_tests.sh

PREFIX ?= /usr/local
BINDIR := $(DESTDIR)$(PREFIX)/bin

# The compiler and the editor extension travel together: by default the
# script also installs syntax highlighting for .duck files.
install: $(BIN)
	install -d $(BINDIR)
	install -m 755 $(BIN) $(BINDIR)/$(BIN)
	@echo "installed $(BINDIR)/$(BIN)"
	@sh editors/vscode/install.sh --optional

uninstall:
	rm -f $(BINDIR)/$(BIN)
	@echo "removed $(BINDIR)/$(BIN)"
	@sh editors/vscode/install.sh --uninstall --optional

# Syntax highlighting for .duck files in VS Code / VSCodium / Cursor.
install-vscode:
	@sh editors/vscode/install.sh

uninstall-vscode:
	@sh editors/vscode/install.sh --uninstall

examples: $(BIN)
	@mkdir -p build
	@for f in examples/*.duck; do \
		echo "  duckc $$f"; \
		./$(BIN) "$$f" -o "build/$$(basename $$f .duck)" || exit 1; \
	done

clean:
	rm -f $(OBJ) $(BIN)
	rm -rf build
