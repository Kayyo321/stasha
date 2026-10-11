# Front-end correctness regressions

Run from any directory:

```sh
scripts/test_frontend.sh
scripts/test_frontend_fuzz.sh
SANITIZERS=address,undefined scripts/test_frontend.sh
SANITIZERS=none scripts/test_frontend.sh
```

The runner compiles a fresh lexer/parser-only executable with `${CC:-cc}` and
requires Python 3 for its 30-second subprocess timeout. It needs no LLVM build.
UBSan is enabled by default; compiler errors, assertion failures, sanitizer
errors, signals, and timeouts all fail the run. On hosts where LeakSanitizer is
unavailable (for example, execution under ptrace), explicitly set
`ASAN_OPTIONS=detect_leaks=0` for the address/undefined run. That run provides no
LeakSanitizer coverage. The tests independently check the compiler's tracked
allocation cleanup.

Both the legacy source parser and the preprocessed-token parser are exercised.
Tests inspect AST values and captured diagnostics, including opening-comment
locations, rather than accepting any nonzero exit as a valid rejection.
Lexical errors are rejected before macro scanning/expansion, so unused internal
or exported function, bare-function, and let macros cannot hide them. Each
lexical error retains its original file/line/column and is reported once.

Ordinary syntax errors at EOF retain the original file and exact 1-based
line/column in both parser modes, including empty input, missing module
semicolons or closing braces, and trailing whitespace or completed comments.
Valid counterparts remain accepted. The preprocessed stream ends with exactly
one original lexer EOF token, with a zero-length span at the source terminator
and no macro expansion origin. Source text and file paths remain caller-owned
through parsing and stream cleanup. Per-file parser tests also switch from a
caller to imported-file context and check that captured diagnostics survive
AST/stream cleanup and release of those caller buffers; they do not run import
resolution or generated programs.

## Integer literal contract

- Decimal and `0x`/`0X` hexadecimal literals contain an unsigned magnitude in
  `uint64_t`, independent of the compiler host's `long` width.
- Magnitudes from zero through 18446744073709551615 are preserved exactly.
  Larger magnitudes are diagnosed before overflowing the host arithmetic.
- This is a bounded representation for the existing i/u8/16/32/64 scalar types,
  **not** an arbitrary-precision compile-time integer implementation.
- A leading zero in a decimal token does not select octal, including in
  fileheader attributes. Hexadecimal literals require at least one hex digit.
- Unary minus remains a separate AST operation, so the magnitude in
  `-9223372036854775808` remains representable. Contextual signedness/width and
  arithmetic validity belong to semantic lowering; parser success alone does
  not assert that a literal fits its declared destination type.
- Existing array dimensions, bitfield widths, alignments, and fileheader fields
  are checked against their current storage types before conversion. Their
  host-sized metadata representation is not yet a target-independent layout
  model, and this suite does not certify allocation sizes or target layout.

## Resource boundaries

Recursive type, expression, statement, and submodule productions share a
256-entry recursion budget. The budget counts guarded grammar productions,
not source characters or parenthesis pairs. Exhaustion emits a source-located
error and ends parsing; speculative rollback cannot resume the failed parse.
Ordinary 16-level nesting remains accepted for each tested syntax. The suite
rejects 1,024-level adversarial nesting without a signal or timeout.

Block-comment nesting is iterative and uses no recursive C stack. Deep valid
comments and long zero-prefixed integer literals remain accepted in linear
scan time. This guard is not a general source-size, preprocessor-expansion,
AST-allocation, or compiler execution budget. Those require separate limits.

The optional fuzz runner generates 256 deterministic mutations with a fixed
seed and runs each in its own subprocess with a two-second timeout. Accepted
syntax and ordinary diagnostic rejection are both valid outcomes. A missing
result marker, sanitizer diagnostic, crash, or timeout fails the run and prints
the reproducer. This small robustness corpus does not establish semantic
correctness or exhaustive parser/preprocessor safety. Minimized regressions
also cover recovery from stray top-level braces and malformed aliases: recovery
must advance, even when the preceding token was a semicolon.

The reviewed implementation also rejects explicit `@align(0)`, empty
`@align()`, and zero-width source bitfields rather than silently interpreting
those tokens as omitted metadata. Compile-time conditional else-if chains use
the same nesting budget as ordinary conditional chains.
