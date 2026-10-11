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

## Literal source positions

Tokens begin at their opening quote, backtick, or dollar prefix, even when their
spelling spans physical lines. Lines count LF bytes exactly once, including
LF after a backslash and LF in CRLF. Columns are 1-based byte offsets: tabs are
one byte, UTF-8 bytes are counted individually, and a lone CR is not a newline.
Whitespace and nested comments use the same physical-line accounting.

These checks preserve existing literal syntax, token kinds, raw byte spans, and
decoded AST values in both parser modes. A backslash followed by physical LF
still decodes to LF in strings and characters; it is not line splicing.
Dollar-quoted identifier contents retain their existing raw spelling. CRLF
remains two bytes and therefore is not a single accepted backtick character.
Unterminated literal errors retain their current-cursor anchor; unterminated
block comments retain their opening-comment anchor.

Regressions assert exact token, following-token, and EOF positions; ordinary
expression errors on continuation lines; and imported-file diagnostics after
source/path cleanup. An exported macro case checks definition-site literal
positions, invocation-site expansion positions, copied expansion text, and
caller-owned file paths through parsing. These are front-end tests, not
end-to-end import resolution or generated-program execution.

## Complete format-placeholder expressions

Both inline `print.(@'...')` placeholders and standalone `@'...'` expressions
must consume the entire expression portion of each slot. An otherwise valid
expression followed by another token, such as `{1 2}`, `{1, 2}`, `{1; 2}`, or
`{1)}`, is rejected rather than silently discarding that token. Existing child
parser errors propagate without an additional trailing-token diagnostic.

The new diagnostic identifies the first unconsumed token in the outer literal,
using its full raw-byte span, physical LF lines, and byte columns. Both parser
modes cover multiline literals, CRLF, escaped physical LF, tabs, UTF-8 bytes,
and imported-file path ownership after cleanup. Exported macros retain their
definition-file location even when expansion copies the literal text and the
parser has returned to the caller's file context.

Valid arithmetic, calls, fields, assignments, pipelines, parenthesized ternaries,
whitespace, completed comments, format specifications, and colon static access
retain their AST structure. After the expression parser has consumed valid
static access, an exact remaining `:x`, `:X`, `:b`, or `:o` is recognized as a
terminal format specification. This preserves standalone formatting and fixes
inline printing, which previously dropped these alphabetic specs. Recognition
does not accept additional text, trivia, unsupported letters, or duplicate
suffixes, and cannot replace a spec already found by the original splitter.

Beyond that bounded terminal-spec correction, quote/comment-aware splitting,
nested-brace grammar, general colon ambiguity, nested interpolation, older
child-parser error remapping, and the two paths' different nesting setup remain
outside this regression contract. Standalone code generation's existing
first-colon interpretation is also unchanged; combined static-access/spec tests
assert parsing and inline rewriting rather than promise new standalone output.

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
