# Stasha for VS Code

## Go to Definition

Go to Definition sends the current file-backed document through the configured
compiler's `definition --stdin --path` interface. Dirty documents are supported;
the provider never saves or reads the caller's disk file. It retains the exact
original text, document identity and version, then derives a separate compiler
snapshot by replacing ordinary CRLF line endings with LF. The exact UTF-8 Buffer
of that admitted snapshot is supplied on every platform. Nothing is rewritten in
the editor or on disk; this is not a claim about the caller's saved encoding.

VS Code's UTF-16 cursor column is converted to a zero-based UTF-8 byte column.
The compiler's nested `{"definition":{"path":...,"line":...,"col":...,"name":...,"kind":...}}`
response is preserved. Returned byte columns are converted back only at exact
Unicode scalar boundaries. Both range endpoints must match a verified source
token: an ordinary ASCII identifier or a complete single-line dollar-quoted name
(including `$` and both quotes). Tabs count as one unit; combining marks remain
separate scalars. No cursor snapping, nearby-name search, clamping, disk fallback
or retry is used. Null results, malformed output and failed processes yield no
location.

### Same-file snapshots and source admission

A same-file result requires exact equality with the caller's absolute path and
uses the known LF-derived compiler snapshot. CRLF-to-LF derivation preserves
logical line count and each line's content before its terminator, so line-local
coordinates map back to the original document. Freshness is always checked
against the complete original text and version, including EOL-only edits and
edit-and-undo. A dirty caller may stay dirty throughout navigation.

This is a bounded supported subset rather than arbitrary source normalization:

- Ordinary LF, CRLF and mixed LF/CRLF separators in code/comments are supported.
  CRLF inside a caller's single/double/dollar-quoted or backtick token is rejected
  because removing CR can change a literal value or name. LF-only multiline
  ordinary strings are not transformed. Quotes inside comments, comment markers
  inside strings, nested block comments and escaped quotes are distinguished.
- Physical backslash+LF and backslash+CRLF in the caller are rejected before
  normalization because of the existing lexer's escaped-newline line accounting.
  This conservative whole-text guard can also reject harmless comment occurrences.
  Literal `\n` escape spelling is unaffected. Physical newlines in backtick
  character tokens and unterminated quotes/block comments are rejected.
- Ill-formed UTF-16, an initial BOM, NUL, Ctrl-Z and bare CR are unsupported.
  Valid U+FFFD is allowed. Unicode is not normalized and tabs are not expanded.
- Positions between surrogate halves, within a multibyte UTF-8 encoding, beyond
  a line or on missing lines are rejected. A query at EOL can be converted, but
  an empty target span cannot produce a location. Untitled, non-file, closed or
  non-host-absolute caller paths are unsupported.

### Imported targets: checked best effort

Different-file targets are supported within one bounded lexical project root:
use the caller's containing file workspace folder, or its directory when there
is no valid containing file workspace. Only the returned host-absolute `.sts`
path is admitted. Parent/sibling/nested targets within that one workspace may
pass; other workspace roots, outside-root paths, sibling-prefix directories,
relative paths and non-`.sts` targets are excluded. The provider checks host path
components and rechecks the selected root across asynchronous work. It does not
search directories, inspect project files, add compiler import paths or probe
realpaths. This is a lexical scope filter, not a symlink-security or canonical
file-identity guarantee. Ordinary dotted module imports remain compiler-defined;
this policy does not add `imp ../sibling` syntax.

Only that returned target is opened and read. Its actual editor document must be
file-backed, open and clean, match the requested file URI, and agree exactly with
strictly round-tripping saved UTF-8 bytes. The target's own unnormalized text is
used for coordinates and token verification. Raw target CRLF is allowed,
including ordinary multiline strings and backslash+CRLF, when complete raw bytes
and editor text agree. Targets retain the bare-CR/BOM/encoding, escaped-LF and
physical-newline backtick restrictions. An editor that normalizes mixed target
EOLs fails the exact comparison. The provider never changes target encoding or
saves it. Its identity, full text, version and clean/open state are rechecked,
and a final target-only byte read must equal the first one.

These checks establish matching **current** target text, not which imported bytes
the compiler parsed earlier. The compiler JSON contains no imported-source digest
or version. Unobserved ABA changes (change and change-back), delayed external
updates and dependency changes remain possible. This branch is checked best
effort, not an atomic cross-file snapshot.

### Remaining limits

- Dollar-quoted target ranges are complete, but the compiler's query hit interval
  still uses the stripped name's byte length from the opening `$`. A body cursor
  in a short quoted name can therefore return no definition. Multiline dollar
  names have unsupported producer locations. The provider does not repair them.
- Token agreement is a consistency check, not proof of semantic resolution,
  macro provenance or all imported/C-header dependency state.
- Cancellation and observed caller/target/root changes are checked after every
  await and before return. The retained synchronous compiler process blocks the
  extension host, so queued editor edits/cancellation may not yet be observable.
  A returned VS Code Location has no version lock; edits after return remain
  possible. A save with no change to guarded state need not invalidate a request.
- LF-only stdin and Ctrl-Z rejection avoid the identified Windows text-mode
  transformations, but native Windows transport is unqualified. Windows-path
  mocks verify provider logic only. Real editor-host behavior is also separate
  from these mocked tests.
- These conversions are specific to Go to Definition; other providers are
  unchanged. The implementation uses APIs available at the VS Code 1.85 floor,
  without newer encoding properties or document-open overloads.

## Provider tests

Run `npm test` from this directory. Node's built-in runner executes the checked-in
`out/definitionProvider.js` with mocked VS Code, compiler lookup/process and
strictly target-only I/O. No editor host or compiler is launched by the suite.
It covers snapshot bytes, dirty callers, scalar boundaries, quoted ranges,
role-specific newline admission, ordinary imported-response paths, workspace
scope, lifecycle invalidation and the existing JSON/process rejection cases.
Mock imports do not qualify native compiler resolution, and matching target
before/after bytes do not prove historical source identity.

After editing TypeScript, regenerate its JavaScript/source map using the existing
TypeScript configuration before testing. `npm run compile` builds the extension;
`node node_modules/typescript/bin/tsc -p . --noEmit` checks types without changing
outputs. A definition-only emit can use TypeScript's `Program.emit(sourceFile)`
with this configuration to regenerate only the paired definition-provider files.
