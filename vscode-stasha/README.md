# Stasha for VS Code

## Go to Definition

Go to Definition invokes the configured Stasha compiler with the current file
path and zero-based cursor line and column. It decodes the compiler's
`{"definition":{"path":...,"line":...,"col":...,"name":...,"kind":...}}`
response; `{"definition":null}`, failed processes, and malformed responses
produce no definition location. The destination range uses `name.length` in
JavaScript (UTF-16 code units), because this response has no `len` field.

### Current limits

- The compiler reads the saved file from disk. Unsaved editor changes are not
  sent to the compiler; save changes before using Go to Definition.
- Compiler columns count bytes, while VS Code character positions count UTF-16
  code units. This decoder preserves columns without conversion. With UTF-8
  source, non-ASCII text before the cursor can prevent a match, and non-ASCII
  text before the declaration can shift the destination to the right. Ordinary
  ASCII names with ASCII text before both positions avoid this mismatch.
- Dollar-quoted names, such as `$"π"`, have their quotes and `$` stripped from
  the returned `name`, but the returned column is at the opening `$`. The
  name-based range is therefore not the full source-token range for these names.

Byte/UTF-16 conversion and unsaved-buffer support require separate work. The
decoder tests characterize the Unicode mismatch rather than claiming Unicode
navigation accuracy.

## Decoder tests

Run `npm test` from this directory. The tests use Node's built-in test runner and
mock VS Code, compiler-path lookup, and `child_process`; they need no test
dependencies and do not launch VS Code or a compiler. They exercise the checked-in
`out/definitionProvider.js`, including the exact ordinary `target`/`caller`
response from `tests/driver/editor_coordinates.py` in the repository root.

After changing TypeScript, regenerate its JavaScript/source map using the
existing TypeScript configuration before running the tests. `npm run compile`
builds the extension, and `node node_modules/typescript/bin/tsc -p . --noEmit`
checks its types without changing build outputs.
