# Finite hosted userspace lifecycle

This test-only Linux x86-64 lane qualifies a bounded in-process fixture. It does
not certify a daemon or whole-process leak freedom, OS resource teardown, general
ownership/lifetime correctness, or real allocator-failure behavior.

The helper processes at most three stack-array requests, checks its one explicit
4-byte allocation for nil, initializes and reads it, and cleans up with defer/rem.
Ordinary success returns 0; intentional request rejection returns 23 unchanged
through real main. Every ordinary image executes once. At O0, O1, O2, O3, all
eight ordinary cases must pass before any observed driver is invoked.

The four observed compiler invocations each launch their image exactly once.
Never run those generated images manually. The observer replaces/excludes root
main, so these results establish helper behavior, separately from ordinary main
startup and normal exit. Each observed run requires two complete tests, four
passing assertions, zero live allocations/outstanding bytes in both allocation
domains, and exactly 20/22 eligible lines hit (bitmap `dfff2f`). The explicit
fixture threshold is 90%. Static new/rem sites are leaf lines 9/16. Guard returns
70 and 24 (lines 11 and 31) are deliberately not exercised.

The fixtures and literal oracles were frozen before compilation. Source and
pre/post-optimization IR must be independently reviewed before first native
execution with a selected compiler. The initial implementation gate uses the
existing nofork compiler for IR collection and pinned LLVM opt for verification;
it is separate from this native runner. A passing build is not that review.

Pure controls (no compiler or native image invocation):

    python3 -m unittest discover -s tests/userspace -p 'test_lifecycle_runner.py'
    python3 -m unittest discover -s tests/harness -p 'test_qualification_script.py'

After the source/IR gate, use the selected installed compiler and matching adjacent
observation_runtime.a without rebuilding:

    python3 tests/userspace/run_lifecycle.py --compiler /absolute/path/to/stasha --opt 0,1,2,3 --artifacts build/qualification/userspace-lifecycle

The mandatory Linux qualification script enrolls the pure control and runner
once before its final compiler hash check and bounded-success receipt. Every
command uses existing process-group timeout cleanup (120 seconds for compiler
calls; 10 seconds for ordinary images). Ordinary status/category and raw stdout
and stderr must match exactly. ANSI-stripped display text is never the oracle.
Ordinary build stdout must be empty, preventing a premature main execution from
being accepted. Compiler error/warning diagnostics also fail even with exit 0;
normal timestamped build logs and observer summaries are allowed on stderr.

Each run gets a private fresh directory and LATEST pointer; a failed rerun removes
any old top-level success receipt. Retained evidence includes exact commands,
status, raw streams, ordinary/observed images, pre/post IR, strict observer JSON
and raw reports, compiler/runtime/source hashes, file identities, and exact case
inventory. Missing, stale, symlinked, malformed, incomplete, replaced or changed
artifacts, duplicate/skipped runs, timeouts or abnormal exits cannot pass.
Working-tree hashes describe review inputs; they do not claim compiler-build
provenance. The initial selected published baseline is commit
`1e69dda33948231ec721eea93b360d741691602c`, tree
`f3310c39bf3d20ace3535b1244256491b19ffc64`; local historical HEAD is not that baseline.

Nonnull use/readback, reviewed allocation routes, cleanup counts and validated
balance support only this finite fixture claim. Ordinary optimization can elide
allocator work: a trace or zero endpoint bytes alone does not prove dynamic
allocator-call counts. No listeners, network, service/privilege changes, new FFI,
threads/signals, restricted transfer/archive probes, invalid-memory/OOM tests,
or additional sanitizer/LSan lane are included.
