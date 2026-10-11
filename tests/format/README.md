# Fixed standalone floating-precision qualification

This suite qualifies the approved bounded `.N` correction on Linux x86-64
LP64 with IEEE binary32/binary64, glibc compact non-finite spellings, the runtime
C numeric locale, and the unchanged default nearest/ties-even environment.
It does not change locale or rounding mode. The C shim queries the environment
and returns ordinary constants through the C ABI; no forged storage, signalling
NaN, division-by-zero construction, descriptor transfer, or archive probe occurs.

## Initial review gates

Source/test review and approval precede **every initial compiler/build/helper
invocation**. Then use the pinned toolchain and separate phases, for example:

```sh
. ../restoration-2026-10-11/toolchain-env.sh
python3 -m unittest discover -s tests/format -p 'test_run_format_tests.py'
python3 tests/format/run_format_tests.py --phase helper --helper-cc "${HOST_CC:-gcc}" --artifacts build/format-review
python3 tests/format/run_format_tests.py --phase ir --compiler bin/stasha-float-precision-r1 --baseline bin/stasha-format-template-r1 --artifacts build/format-review
```

The IR phase emits candidate IR at O0/O2, compares the ordinary inline fixture
byte-for-byte with pinned A when `--baseline` is supplied, and runs four
compile-only rejections. It never links or executes a generated program.
Independently inspect the retained O0 IR for all selected conversions/capacities,
exactly one f32 extension in `promote_once`, none in `direct_f64`, one snprintf
per standalone site, once-only left-to-right calls, entry stack allocations,
heap minima/cleanup, and exact ordinary inline preservation. The two focused
promotion functions also have mechanical fixed-capacity checks (329/328 bytes).

Only after independent IR approval:

```sh
python3 tests/format/run_format_tests.py --phase native --compiler bin/stasha-float-precision-r1 --baseline bin/stasha-format-template-r1 --cc "$CC" --ir-receipt build/format-review/REPLACE-WITH-IR-RUN/results.json --artifacts build/format-review
```

The native gate binds current compiler, source/test/oracle identities and all
four retained candidate IR hashes to that successful receipt. It also verifies
the baseline inline IR hashes when present. The receipt records evidence; it
does not itself grant independent review approval. An absent/failed/stale or
incomplete gate stops before support compilation or renderer execution.

For routine qualification of an already reviewed revision,
`--phase all --compiler PATH --cc PATH --helper-cc HOST_CC --artifacts DIR` runs helper, IR and
native in order, stopping on failure. The hosted qualification script enrolls
this once before its success receipt. `--baseline` is optional for ordinary CI
because frozen historical compilers are not a general prerequisite; initial
B review requires it. Its exact allowed SHA-256 is
`782c3005a127ccdb6a0c5ac39b77562f5ebb77a242e7e5534f4db8e8b00f15bd`.
Only the ordinary **inline** fixture can be compiled or executed with it.
The broken standalone baseline precision path is never executed.

## Fixed oracle and coverage

`float_precision_cases.json` freezes complete UTF-8 stdout bytes before any
candidate execution. Finite maxima are computed with integer arithmetic:
`(2**53-1)*2**971` and `(2**24-1)*2**104`; the runner independently checks those
stored decimal strings without calling a formatter. `.2` appends `.00` to the
exact integer; negative controls prepend `-`. Quiet NaN/infinity spellings are
separate glibc-specific `nan`, `inf`, and signed/padded controls, not a universal
finite-bound theorem. No expected value comes from candidate or baseline output.

Runtime precision is at most 20 and width at most 40. The fixed fixtures cover:

- Stored f32 versus f64 `0.1` at `.9`; exact precision, ties and carry
- Positive, negative, zero, negative zero, subnormal and finite extrema
- Width, zero padding, sign, left alignment, composed/leading-zero flags;
  `#` is parsed but ignored, so `#.0` does not add a radix point
- Stack, explicit heap and returned heap; static/chained calls and an alias
- Multiple side effects once in left-to-right order, with exact `trace=123`
- Multiple slots, percent, braces, escapes and physical source lines
- Default floats, bounded i32/hex mixtures, empty strings, classic and inline
  controls including the retained inline integer-to-float precision behavior

`format_spec_tests.c` is the pure fixed helper oracle: parse selection, checked
numeric fields, exact complete conversion strings/capacity/canaries, maximum
conversion length, range precedence, output/scratch/count bounds and minima.
The runner builds and executes it in plain C, UBSan, and ASan+UBSan.
`--helper-cc` defaults to `HOST_CC` or `gcc` for these pure helper builds;
`--cc` remains the pinned Clang for the native C shim. Both executable identities
are recorded separately. Other hosted suites keep their existing compiler choices. Every helper
run uses `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`; no LeakSanitizer coverage is
claimed and no sanitizer failure is retried under weaker settings.

Four short named fixtures are always compile-only, even on unexpected success:

| Fixture | Primary diagnostic | Real outer anchor |
| --- | --- | --- |
| precision_overflow | format precision exceeds 2147483647 | line 4 (column unavailable) |
| width_overflow | format width exceeds 2147483647 | line 4 (column unavailable) |
| precision_bound | comptime format output bound exceeds 2147483647 bytes | line 4 (column unavailable) |
| width_bound | comptime format output bound exceeds 2147483647 bytes | line 4 (column unavailable) |

Each requires exit 1, exactly its primary diagnostic, its actual source anchor,
no unrelated primary diagnostic, unchanged input and no candidate output.
The outer format AST currently has column zero, so the exact diagnostic anchor
is the fixture filename plus `:4`, with no fabricated column. The established
driver logging directory `logs` and exactly `logs/.rotation-lock` and
`logs/log0-last.log` are allowed as ordinary regular files/directories. Other
paths, candidate artifacts and symlink substitutions fail. Giant
valid values are helper arithmetic only, never generated arrays or libc output.
Unsupported raw spec spellings remain metadata-only helper cases.

## Evidence and failure behavior

Each phase gets a fresh directory with exact commands, cwd, timeout, environment
overrides, separate raw streams, exit/category, compiler/C-compiler/source/oracle
identities (native C and helper compilers separately), and artifact hashes. Commands use the established bounded harness;
positive runtime stdout is compared as raw bytes and stderr must be empty.
An ICE, sanitizer finding, timeout, signal, wrong output, unexpected success,
missing output, changed input/compiler, or stale IR fails closed. Missing required
tools fail rather than skip. Existing successful outputs are never reused.

`test_run_format_tests.py` checks those rules with mocked results only; it does
not qualify any real compiler. The shell enrollment controls prove formatting
failure removes the previous PASS receipt and stops later stages.

The tests do not prove general format validation, wide-integer/raw-spec or
pointer-length completeness, binary portability, all locales/libcs, allocation
success, or a new snprintf runtime-error policy. Other targets remain unqualified.
