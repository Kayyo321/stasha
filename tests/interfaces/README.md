# Interface I0: retained metadata and declaration graphs

This stage validates interface topology and retains source metadata needed by
later signature work. It does **not** qualify complete implementation matching,
boxing eligibility, dynamic-call ABI correctness, object lifetime or memory
safety, or runtime CFI.

## Contract

- AST-arena-owned type spans, written return storage, raw nullable markers,
  generic constraints, and recursively structured generic applications survive
  shallow AST copies. Literal `_G_` names have no parsed-application provenance.
- Each `codegen()` invocation owns its graph cache and origin arrays. Cache
  teardown does not free borrowed AST declarations. No static graph state is
  reused across modules/compilations.
- Parent order is source order, inherited first. One originating declaration is
  retained once across a diamond. Distinct declarations with the same method
  name remain separate origins in one pending group; graph readiness does not
  imply signature readiness. A table/call consuming multiple distinct origins
  diagnoses unsupported pending signature validation, even if their source
  signatures happen to look equal.
- Limits are 8 direct parents/implemented interfaces, 32 own methods, 32 unique
  flattened method names, and 64 interfaces on an inheritance path. Cached
  longest-path heights enforce depth independently of declaration order.
- Interface-affected return storage and generic constraints/interfaces diagnose
  unsupported contracts. Ordinary non-interface return annotations, including
  `?heap T *r`, and unconstrained generics retain their existing behavior.
- Metadata inspection follows relevant aliases without canonical type equality.
  Cycles and nesting beyond 256 are diagnosed. Nullable slice/callback/closure
  syntax retains its raw marker and is unsupported in interface contracts.
- Formatted interface names exceeding 511 bytes diagnose before truncation,
  including late table requests. Pointer depth beyond 8 is consumed/rejected
  normally; callback parameter bounds remain 32.
- Empty interface declarations are accepted; constructing a table for an empty
  interface is explicitly unsupported in I0.
- Diagnostic source snapshots are owned, replaceable by filename, and explicitly
  cleared at compilation teardown. Stable filenames/coordinate-based captured
  diagnostics outlive snapshot cleanup. Unknown files cannot show stale snippets.

## Archive compatibility

I0 uses semantic ABI marker **6**. Diamond deduplication changes slot indices even
though descriptor bytes remain `{ptr, ptr}`. Rebuild all source-backed archives.
ABI marker compatibility alone does not establish signature or memory safety.

## Running

```
source ../env.sh
python3 tests/interfaces/run_i0.py \
  --compiler bin/stasha-interface-i0-r4 --artifacts /tmp/i0-current
python3 tests/interfaces/test_i0_runner.py
python3 tests/interfaces/test_i0_units.py \
  --llvm-build build/llvm --artifacts /tmp/i0-units
python3 tests/interfaces/test_i0_archives.py \
  --compiler bin/stasha-interface-i0-r4 \
  --b1-compiler bin/stasha-make-b1-r5 --artifacts /tmp/i0-archives
```

`i0_inventory.json` enumerates the 80 mandatory current-compiler controls.
Missing, extra, duplicate or empty inventory entries fail the gate. Supplying
`--b1-compiler bin/stasha-make-b1-r5` to the ordinary runner also delegates to the
strict legacy/current archive controls and produces 81 top-level checks.

The frozen B1 source publication is `e8a1d04fe3cf64aed321139571c461c5977163c9`,
source tree `d768d467a665eae676923d451654e70e915702f0`. Its preserved compiler SHA-256
must be `c1fb7d98ae03a1d4b783f7b8c0a03b6df22346ac471f46f7190947049ab832dc`.
The archive runner rejects any other legacy compiler hash. Preserve that binary
before rebuilding the current source; the test never fabricates an ABI5 marker
on a current archive. Its generated source fixtures and exact build commands are
retained in the requested artifacts directory for reruns.

The ordinary runner explicitly reports omission of the genuine legacy control
when no frozen B1 compiler is supplied. Local final qualification requires the
separate archive runner. Its B1 SHA is pinned. The legacy compiler creates the
archive then returns 1 because of two old registry leaks (25,344 and 256 bytes).
That provider cleanup is recorded as FAIL, with a narrow expected degraded
fixture profile; it is never reported as a clean build. Member/symbol inspection,
ABI5 sidecar, legacy five-slot IR and current four-slot IR establish provenance.
The ABI5 consumer is compile-only and must reject. The ABI6 consumer constructs
a current table through an already-supported ordinary-call boxing route, passes
it into the provider, and exercises inherited dispatch on both sides.

The compiler itself is built with pinned Clang/LLVM 23. Probe compilation uses
host GCC/G++ with ASan+UBSan against the pinned LLVM 23 libraries because this
Clang build lacks compiler-rt sanitizer runtimes. Exact commands/versions are
saved. LeakSanitizer cannot run under this executor's ptrace environment, so its
coverage is explicitly excluded; direct lifecycle checks cover registry clearing,
re-registration, stable locations and repeated contexts. Sanitizer crashes are
failures, not accepted diagnostics or silently retried passes.

Invalid fixtures are never executed, even if accepted unexpectedly. Rejection
requires normal exit 1, expected error text plus relevant source coordinates,
no artifact (including stale/symlink output), bounded completion and no internal,
sanitizer, verifier or warning output. Positive native fixtures require exact
zero exit and empty runtime output. Compiler output is limited to ordinary
progress logs. The tests create source and evidence in unique directories.

The graph unit invokes codegen four times in one process with reused interface
names and different slot layouts; this is separate from repeated CLI tests.
Import tests use explicit root imports and do not claim transitive import
resolution has been repaired. The broad hosted gate and rebuilt stdlib remain
separate required regression families, coordinated by the publication owner.
