#!/usr/bin/env bash
# Bounded hosted compiler qualification, not full native/kernel release approval.
set -euo pipefail
cd "$(dirname "$0")/.."
compiler="$(realpath "${1:-bin/stasha}")"
artifacts="$(realpath -m "${2:-build/qualification}")"
mkdir -p "$artifacts"
# Invalidate a previous success before any prerequisite or suite can fail.
rm -f "$artifacts/RESULT.txt"
printf 'RUNNING source=%s\n' "$(git rev-parse HEAD)" > "$artifacts/STATUS.txt"
trap 'code=$?; if (( code != 0 )); then printf "FAIL exit=%s\n" "$code" > "$artifacts/STATUS.txt"; fi' EXIT
llvm_build="$(realpath "${LLVM_BUILD:-build/llvm}")"
[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || { echo 'Requires Linux x86-64' >&2; exit 1; }
mkdir -p "$artifacts"
python3 scripts/verify_llvm_toolchain.py --llvm-build "$llvm_build" --require-clang --output "$artifacts/toolchain.json"
sha256sum "$compiler" > "$artifacts/compiler.sha256"
"$compiler" --version > "$artifacts/compiler-version.txt"
bash scripts/test_harness_selftest.sh
python3 -m unittest discover -s tests/driver -p 'test_project_cli.py'
python3 -m unittest discover -s tests/driver -p 'test_editor_coordinates.py'
python3 -m unittest discover -s tests/driver -p 'test_target_cli.py'
python3 -m unittest discover -s tests/format -p 'test_run_format_tests.py'
python3 tests/harness/compiler_suites.py diag "$compiler" --artifacts "$artifacts/diag" --keep-artifacts
python3 tests/harness/compiler_suites.py signals "$compiler" --artifacts "$artifacts/signals" --keep-artifacts
python3 tests/harness/compiler_suites.py cinterop "$compiler" --llvm-config "$llvm_build/bin/llvm-config" --require-prerequisites --artifacts "$artifacts/cinterop" --keep-artifacts
python3 tests/scalars/run.py --compiler "$compiler" --opt 0,1,2,3 --build-trap-hook --require-trap-reasons --artifacts "$artifacts/scalars"
python3 tests/memory/run_bounds_a1.py --compiler "$compiler" --cc "${CC:-clang}" --opt 0,1,2,3 --artifacts "$artifacts/bounds-a1"
python3 tests/memory/run_make_b1.py --compiler "$compiler" --cc "${CC:-clang}" --opt 0,1,2,3 --artifacts "$artifacts/make-b1"
python3 tests/memory/run_array_queries.py --compiler "$compiler" --opt 0,1,2,3 --artifacts "$artifacts/array-queries"
# Genuine historical archive controls require separately frozen compilers and
# run in local checkpoint qualification. This CI stage does not claim them.
python3 tests/interfaces/run_i0.py --compiler "$compiler" --artifacts "$artifacts/interfaces-i0"
python3 tests/interfaces/run_i1a.py --compiler "$compiler" --artifacts "$artifacts/interfaces-i1a"
python3 tests/interfaces/run_i1b.py --compiler "$compiler" --artifacts "$artifacts/interfaces-i1b1"
python3 tests/interfaces/i1b2_run.py --compiler "$compiler" --artifacts "$artifacts/interfaces-i1b2"
python3 tests/interfaces/i1b3a_run.py --compiler "$compiler" --artifacts "$artifacts/interfaces-i1b3a"
python3 tests/interfaces/factory_result_run.py --compiler "$compiler" --phase gate-b --artifacts "$artifacts/interfaces-factory-results"
python3 tests/interfaces/descriptor_defaults_run.py --compiler "$compiler" --artifacts "$artifacts/interfaces-descriptor-defaults"
python3 tests/format/run_format_tests.py --compiler "$compiler" --cc "${CC:-clang}" --helper-cc "${HOST_CC:-gcc}" --phase all --artifacts "$artifacts/format-precision"
python3 tests/layout/run_layout_tests.py --compiler "$compiler" --cc "${CC:-clang}"
python3 tests/abi/run_sysv_aggregates.py "$compiler" --cc "${CC:-clang}" --out "$artifacts/abi"
python3 tests/abi/run_c_callbacks.py "$compiler" --cc "${CC:-clang}" --out "$artifacts/callbacks"
python3 tests/abi/run_c_callback_parameters.py "$compiler" --cc "${CC:-clang}" --out "$artifacts/callback-parameters"
python3 tests/abi/run_member_pointers.py --compiler "$compiler" --cc "${CC:-clang}" --artifacts "$artifacts/members"
python3 tests/cleanup/run.py --compiler "$compiler" --opt 0,2 --zone-probe --artifacts "$artifacts/cleanup"
python3 tests/driver/run.py --compiler "$compiler" --artifacts "$artifacts/driver"
python3 tests/driver/project_cli.py --compiler "$compiler" --artifacts "$artifacts/project-cli"
python3 tests/driver/editor_coordinates.py --compiler "$compiler" --artifacts "$artifacts/editor-coordinates"
python3 tests/driver/target_cli.py --compiler "$compiler" --artifacts "$artifacts/target-cli"
python3 tests/driver/global_initializers.py --compiler "$compiler" --artifacts "$artifacts/globals"
python3 tests/observation/test_observation.py --compiler "$compiler" --cc "${CC:-clang}" --opt 0,1,2,3 --artifacts "$artifacts/observation"
sha256sum --check "$artifacts/compiler.sha256"
printf 'PASS source=%s\n' "$(git rev-parse HEAD)" > "$artifacts/STATUS.txt"
printf '%s\n' 'PASS: bounded Linux hosted compiler qualification; see native/kernel qualification limits.' > "$artifacts/RESULT.txt"
