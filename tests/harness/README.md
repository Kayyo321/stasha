# Compiler test oracles

These runners require Python 3 and Bash. They do not require third-party Python
packages. Run from the repository root; compiler paths are resolved relative to
the caller before compilation is run from the repository root (required by the
fixtures' relative header search paths).

```sh
bash scripts/test_harness_selftest.sh
bash tests/diag/run_diag_tests.sh ./bin/stasha
make tests/cinterop/build/libcinterop_support.a
bash tests/cinterop/run_cinterop_tests.sh ./bin/stasha
STASHA=./bin/stasha bash tests/signals/run_tests.sh
bash tests/cinterop/run_cinterop_tests.sh ./bin/stasha \
  --filter '^llvm_smoke$' --require-prerequisites
```

`--list` prints the explicit JSON case inventory. `--filter REGEX` selects case
names; selecting nothing fails. There is no implicit discovery of `.sts` files
as positive programs. The C-interop inventory now includes the previously
unlisted `14_va_access`, `15_va_foreach`, and `16_va_typed` cases. Diagnostic
fixtures outside the explicit inventory remain unqualified, not silently passed.

## Outcome contracts

- **Compile success:** exit 0, no error/internal/verifier/sanitizer diagnostic,
  and a regular executable produced in the new case directory. Every case has a
  new output path; checked-in or previously produced binaries are never used.
- **Expected compile rejection:** exactly exit 1 (`Err` in `common.h`), the
  expected diagnostic text on stderr, a source filename and nonzero line
  location, no internal/verifier/sanitizer failure, and no final output artifact.
  Wrong exit codes, signals, exceptions, and timeouts are independent failures.
- **Runtime success:** the case's exit code and stdout contract must both match.
  C-interop retains its substring checks. Signals retains exact text/order
  checks, ignoring only trailing newlines as the original shell runner did.
  `quit_from_exit` explicitly requires runtime exit 1; it is not a compile-negative
  case. Warning-only diagnostic cases do not execute their `main` functions.
- **XFAIL:** `xfail/04b_struct_byval_arg` is a known macOS/AArch64 aggregate-argument
  ABI failure. Only successful compilation followed by runtime exit 1 and the
  fixture's exact wrong-value marker counts as XFAIL on that host. Other failures
  fail. Unexpected success is XPASS and fails so the exception gets reviewed.
  On other hosts the fixture is a normal required-positive case.
- **SKIP:** missing in-tree LLVM build can skip only the optional C-interop
  `llvm_smoke` row, with the reason printed. `--require-prerequisites` makes that
  a failure, as used by `make test-llvm-smoke`. Missing fixtures and the C support
  archive are failures. No skip counts as a pass.

The legacy `coro_await_outside_async` rejection assertion is deliberately retained;
it conflicts with newer documentation allowing await outside async functions.
The runner must expose such semantic drift rather than silently change the
language contract or fabricate an xfail. Real compiler execution/review is needed.

## Timeouts and artifacts

Defaults are 60 seconds per compilation and 15 seconds per execution. Override
with `--compile-timeout`/`--run-timeout`, or `STASHA_TEST_COMPILE_TIMEOUT` and
`STASHA_TEST_RUN_TIMEOUT`. Values must be finite and positive. POSIX timeouts kill
the full child process group, including spawned linkers; Windows uses `taskkill /T`
and a final process kill. Platform branches require qualification on their hosts.

Every run creates fresh per-case temporary directories. Failures always retain
artifacts and print their path. Use `--keep-artifacts` to retain successful runs,
or `--artifacts DIR` to create and retain the new run directory under `DIR`.
Artifacts include:

- Separate original stdout/stderr and JSON process records with exact commands,
  working directory, explicit environment overrides, exit code/category, timeout,
  and elapsed time
- Pre-/post-optimization IR when the compiler reaches and honors the existing
  `STS_DUMP_IR_PRE`/`STS_DUMP_IR` hooks, plus any surviving object/executable
- `results.json` with expected/actual outcomes, fixture and compiler SHA256,
  source revision/dirty state, timestamp, host, and Python version

An absent IR dump does not establish that emission was safe; some errors occur
before those hooks. Full toolchain provenance belongs to the build record.

## Oracle selftests versus language qualification

The selftests use a mock compiler in an isolated fixture checkout, including a
planted stale executable with an execution marker. They verify that deliberately
broken output, nonzero compilation with a fresh output, stale outputs, timeouts,
crashes, sanitizer/verifier output, incorrect negative status/location, runtime
errors, and invalid xfail signatures are caught. The timeout test also checks
that a descendant does not survive to write its marker. Successful mock tests
prove the harness contracts, not that Stasha compiles any language feature.

The mock executable/scripts are exercised on POSIX with Bash. For MSYS2 CI,
install its `python` package alongside Bash; native Windows Python and process
handling still require separate platform verification.

## Language test-block execution

`python3 tests/harness/run_language_tests.py bin/stasha FILE.sts [-l LIB]`
compiles into a fresh retained directory, then executes that new program. A
successful compile alone cannot pass: the runtime must exit zero and emit
exactly one nonempty `N passed, 0 failed` summary. Timeouts, crashes, sanitizer
reports, missing output and empty suites fail. Make's stdlib-test/test-threads
use this runner. This corrects the old compile-only test gate.


The LLVM static-library capstone includes `llvm-config --system-libs` and its
matching C++ runtime. On GNU/Linux the harness resolves `libstdc++.so` through
`CXX` (default `c++`); `--cxx-runtime PATH_OR_LINKER_TOKEN` explicitly overrides
this for other builds. Missing runtime resolution fails rather than silently
skipping the capstone. This supplies test-link dependencies, not a compiler
calling-convention exception.
