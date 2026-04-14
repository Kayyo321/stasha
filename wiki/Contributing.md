# Contributing

Stasha is open to contributions. Here's how to get started.

---

## Getting the Source

```bash
git clone https://github.com/YOUR_USERNAME/stasha.git
cd stasha
make
```

---

## Repository Structure

```
src/
  main.c              CLI, import resolver
  common/             logging, heap tracking, utilities
  lexer/              tokenizer
  ast/                AST node definitions
  parser/             recursive-descent parser
  preprocessor/       macro expansion
  codegen/            LLVM IR generation + safety checks
  runtime/            thread pool + zone allocator
  linker/             LLD wrapper
  tooling/            editor/LSP support

src.bootstrap/        Self-hosting bootstrap compiler (written in Stasha)
  lex/               Lexer (complete)
  main.sts

stsstdlib/            Standard library (written in Stasha)
  collections/
  str/
  math/
  io/
  ...

examples/             Example programs (great for testing new features)
extlib/               Bundled C libraries (cJSON, mongoose, OpenSSL)
std/                  C wrappers for stdlib modules
```

---

## Building and Testing

```bash
make                  # build the compiler
make stdlib           # build all stdlib modules
make stdlib-test      # run stdlib tests
stasha test examples/ex_types.sts    # test individual examples
```

---

## Adding a Language Feature

1. **Lexer** (`src/lexer/lexer.h`, `lexer.c`) — Add the token(s)
2. **Parser** (`src/parser/parse_*.c`) — Add grammar rules
3. **AST** (`src/ast/ast.h`) — Add node kinds if needed
4. **Codegen** (`src/codegen/cg_*.c`) — Emit LLVM IR
5. **Example** (`examples/`) — Write a `.sts` example
6. **Test** — Add `test 'name' { }` blocks to the example

---

## Adding a Stdlib Module

1. Create `stsstdlib/<category>/mymodule.sts`
2. Start with `mod <category>.mymodule;`
3. Export public types/functions with `ext`
4. Add tests in the same file with `test 'name' { }`
5. Add a build rule to the Makefile
6. Run `make stdlib-test`

---

## Code Style

The compiler is written in C (C2x standard). Follow the existing style:
- 4-space indentation
- `snake_case` for variables and functions
- `UPPER_CASE` for constants and macros
- Structs named `thing_t`

Stasha source follows the same conventions.

---

## Submitting Changes

1. Fork the repository
2. Create a feature branch: `git checkout -b feature/my-feature`
3. Make your changes
4. Test: `make && stasha test examples/`
5. Open a pull request with a clear description

---

## Good First Contributions

- Add examples for undocumented features
- Write stdlib module tests
- Improve error messages in the compiler
- Add a stdlib module in a missing area
- Work on the bootstrap compiler (`src.bootstrap/`)
- Improve the wiki (you're reading it!)
