#!/usr/bin/env python3
"""Independent observation qualification. Run only after the source/IR gate.

Literal line oracles below are hand-authored from fixtures, never learned from
compiler output. Only defined-behavior native programs are launched. Invalid
allocator events are confined to the separate opaque-ID allocator-spy unit test.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).resolve().parent / "fixtures"
COVERAGE_LINES = [3, 4, 5, 6, 7, 8, 10, 11, 12, 14]
BALANCED_LINES = [3, 4, 5, 6, 7]
CONTROL_LINES = [3, 4, 5, 6, 7, 8, 10, 11, 13, 14, 15, 16, 17, 18, 19, 21,
                 23, 24, 25, 26, 27, 28, 29, 30, 32, 33, 34, 36, 37]

# These namespace controls are metadata for the separately hashed, fork-disabled
# IR staging compiler only. The native qualification path must never launch them.
COMPILE_ONLY_CONTROLS = {
    "internal-runtime-names": {
        "source": "mod internal_names;\nstack i32 sigaction = 5;\nfn open(void): i32 { ret 3; }\nfn write(void): i32 { ret 4; }\ntest 'source names remain local' { expect_eq.(open() + write() + sigaction, 12); }\n",
        "pre_ir_patterns": [r"^@sigaction = internal global ",
                            r"^define internal i32 @open\(",
                            r"^define internal i32 @write\("],
    },
}

REJECTIONS = {
    "llvm-intrinsic-name": 'fn $"llvm.trap"(void): void { }',
    "exotic-binding-name": 'stack i32 $"has.dot" = 0;',
    "long-binding-name": "fn " + "n" * 128 + "(void): void { }",
    "runtime-hook-collision": "fn __stasha_obs_init(void): void { }",
    "runtime-counter-collision": "stack i32 __stasha_obs_assert_pass = 0;",
    "harness-collision": "fn __test_0(void): void { }",
    "main-variable-collision": "stack i32 main = 0;",
    "wildcard-guard": "fn unused(void): void { match 1 { _ if true => { } } }",
    "raw-native-call": "fn unused(void): void { malloc(8); }",
    "dead-native-call": "fn unused(void): void { ret; free(nil); }",
    "dead-pointer-cast": "fn unused(void): void { ret; heap u8 *rw p = (u8 *rw) 1; }",
    "function-value": "fn unused(void): void { ret; stack u64 x = main; }\nfn main(void): i32 { ret 0; }",
    "external-function": "ext fn foreign(void): i32;",
    "type-struct": "type Object: struct { ext i32 value; };",
    "type-union": "type Object: union { i32 value; f32 other; };",
    "type-enum": "type Object: enum { A, B };",
    "slice": "fn unused(void): void { stack []i32 value; }",
    "pointer-array": "fn unused(void): void { stack u8 *rw values[2]; }",
    "heap-format": "fn unused(void): void { heap i8 *rw value = heap @'value'; }",
    "asm": "fn unused(void): void { asm { 'nop' } }",
    "unsafe": "fn unused(void): void { unsafe { stack i32 value = 1; } }",
    "unchecked": "fn unused(void): void { stack i32 a[2] = .{1, 2}; stack i32 x = a[unchecked: 0]; }",
    "zone": "fn unused(void): void { zone arena; }",
    "async": "async fn unused(void): i32 { ret 1; }",
    "thread": "fn work(void): i32 { ret 1; }\nfn unused(void): void { stack future f = thread.(work)(); }",
    "lambda": "fn unused(void): void { stack fn*(stack i32): i32 f = lam.(stack i32 n): i32 { ret n; }; }",
    "generic": "fn @comptime[T] unused(stack T value): T { ret value; }",
    "lifecycle": "@[[init]] { print.('INITIALIZER_MUST_NOT_RUN'); }",
    "variadic": "fn unused(stack i32 n, ...): void { }",
    "atomic": "fn unused(void): void { atomic i32 value = 1; }",
    "macro": "int macro let unused! = 7;",
    "import": "imp deliberately_missing;",
    "library": "lib 'deliberately_missing';",
    "libimp": "libimp 'deliberately_missing' from std;",
    "cheader": "cheader 'deliberately_missing.h';",
    "header": "@[[weak]] fn unused(void): i32 { ret 1; }",
}


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Qualification:
    def __init__(self, compiler: Path, artifacts: Path):
        self.compiler = compiler.resolve()
        artifact_root = artifacts.resolve()
        artifact_root.mkdir(parents=True, exist_ok=True)
        self.artifacts = Path(tempfile.mkdtemp(prefix="run-", dir=artifact_root))
        (artifact_root / "LATEST").write_text(str(self.artifacts) + "\n")
        self.records = []
        self.run_ids = set()
        self.compiler_hash = sha(self.compiler)
        self.runtime = self.compiler.parent / "observation_runtime.a"
        self.runtime_hash = sha(self.runtime)
        paths = set(ROOT.glob("src/**/*.c")) | set(ROOT.glob("src/**/*.h"))
        paths |= set((ROOT / "tests/observation").glob("*.c")) | set(FIXTURES.glob("*.sts"))
        paths |= {Path(__file__).resolve(), ROOT / "Makefile", ROOT / "std/crypto/sha256_rt.c", ROOT / "std/crypto/sha256_rt.h"}
        self.source_manifest = {str(path.relative_to(ROOT)): sha(path) for path in sorted(paths)}
        self.status = "running"
        self.failure = None
        (self.artifacts / "source-manifest.json").write_text(json.dumps(self.source_manifest, indent=2) + "\n")

    def verify_inputs(self):
        assert sha(self.compiler) == self.compiler_hash, "compiler changed during qualification"
        assert sha(self.runtime) == self.runtime_hash, "runtime changed during qualification"
        for relative, expected in self.source_manifest.items():
            assert sha(ROOT / relative) == expected, f"source changed during qualification: {relative}"

    def receipt(self):
        (self.artifacts / "result.json").write_text(json.dumps({"status": self.status, "failure": self.failure,
            "compiler_sha256": self.compiler_hash, "runtime_sha256": self.runtime_hash,
            "source_manifest_sha256": sha(self.artifacts / "source-manifest.json"), "runs": self.records}, indent=2) + "\n")

    def command(self, name: str, argv: list[str], *, env=None, expected=0, cwd=None):
        dest = self.artifacts / name
        dest.mkdir(exist_ok=False)
        (dest / "command.json").write_text(json.dumps(argv, indent=2) + "\n")
        self.verify_inputs()
        try:
            result = subprocess.run(argv, cwd=cwd or ROOT, env=env, capture_output=True, timeout=60)
        except subprocess.TimeoutExpired as error:
            (dest / "stdout").write_bytes(error.stdout or b"")
            (dest / "stderr").write_bytes(error.stderr or b"")
            self.records.append({"name": name, "command": argv, "timeout": True})
            raise
        self.verify_inputs()
        (dest / "stdout").write_bytes(result.stdout)
        (dest / "stderr").write_bytes(result.stderr)
        (dest / "status.json").write_text(json.dumps({"returncode": result.returncode, "expected": expected}) + "\n")
        self.records.append({"name": name, "command": argv, "returncode": result.returncode})
        assert result.returncode == expected, f"{name}: unexpected status {result.returncode}: {result.stderr.decode(errors='replace')}"
        return dest, result

    def observed(self, name, source, flags, opt=0, *, expected=0, lines=None, covered=None,
                 byte_count=None, live=None, assertions=None, sentinel=False, child_exit=0, tests=1):
        source_path = Path(source)
        source_before = sha(source_path)
        env = dict(os.environ)
        output = self.artifacts / (name + ".test")
        env["STS_DUMP_IR_PRE"] = str(self.artifacts / (name + ".pre.ll"))
        env["STS_DUMP_IR"] = str(self.artifacts / (name + ".post.ll"))
        dest, result = self.command(name, [str(self.compiler), "test", str(source_path), "-o", str(output),
                                          f"-o={opt}", *flags], env=env, expected=expected)
        if sentinel:
            assert result.stdout.count(b"OBSERVATION_ONCE\n") == 1, f"{name}: test image not run exactly once"
        match = re.search(rb"observation: private evidence ([^\r\n]+)", result.stderr)
        assert match, f"{name}: no independent evidence location"
        evidence = Path(os.fsdecode(match.group(1)))
        data = json.loads((evidence / "driver.json").read_text())
        shutil.copy2(evidence / "driver.json", dest / "driver.json")
        if (evidence / "report").exists():
            shutil.copy2(evidence / "report", dest / "report")
        assert sha(source_path) == source_before and data["source_sha256"] == source_before
        assert data["runtime_sha256"] == self.runtime_hash
        assert data["compiler_sha256"] == self.compiler_hash
        assert data["run"] not in self.run_ids, f"{name}: stale run ID"
        self.run_ids.add(data["run"])
        assert data["report_valid"] and data["complete"] == 1 and data["suite_endpoint"] == 1
        assert data["child"]["launched"] and data["child"]["waited"]
        assert data["child"]["exited"] and data["child"]["exit_code"] == child_exit
        assert data["child"]["signal"] == 0 and data["child"]["launch_errno"] == 0 and data["child"]["wait_errno"] == 0
        assert data["overall_status"] == expected and data["compile_link_success"] and data["manifest_valid"]
        assert len(data["tests"]) == tests
        allocation_enabled = bool(data["enabled"] & 1)
        coverage_enabled = bool(data["enabled"] & 2)
        assert (data["allocation_metric"] == "disabled") == (not allocation_enabled)
        assert (data["coverage_metric"] == "disabled") == (not coverage_enabled)
        if allocation_enabled:
            maximum = data["max_leaked_bytes"]
            assert data["allocation_metric"] == ("pass" if maximum is None or data["tracked_outstanding_bytes"] <= maximum else "fail")
        if coverage_enabled:
            eligible = len(data["lines"])
            passes = eligible > 0 and data["covered_lines"] * 100 >= data["min_test_coverage"] * eligible
            assert data["coverage_metric"] == ("pass" if passes else "fail")
        assert data["binary_sha256"] == sha(output)
        if lines is not None: assert data["lines"] == lines, (name, data["lines"], lines)
        if covered is not None: assert data["covered_lines"] == covered, (name, data["covered_lines"], covered)
        if byte_count is not None: assert data["tracked_outstanding_bytes"] == byte_count, name
        if live is not None: assert data["live_allocations"] == live, name
        if assertions is not None: assert (data["assertions_passed"], data["assertions_failed"]) == assertions, name
        # Independent bitmap parsing verifies each source line rather than a child percentage.
        report = (dest / "report").read_text().splitlines()
        assert next(row for row in report if row.startswith("tests ")) == f"tests {tests} {tests}"
        assert [row for row in report if row.startswith("test ")] == [f"test {i} 2" for i in range(tests)]
        assert next(row for row in report if row.startswith("inventory ")) == f"inventory {tests} {len(data['lines'])} {len(data['sites'])}"
        bitmap = next(row.split(" ")[1] for row in report if row.startswith("hits "))
        hits = bytes.fromhex(bitmap) if bitmap != "-" else b""
        uncovered = [line for i, line in enumerate(data["lines"]) if not hits[i // 8] & (1 << (i % 8))]
        data["uncovered_oracle"] = uncovered
        if flags and "--min-test-coverage" in flags:
            assert len(data["lines"]) - len(uncovered) == data["covered_lines"]
        return data

    def generated_source(self, name, body):
        path = self.artifacts / (name + ".sts")
        path.write_text(body)
        return path

    def run(self, levels, cc):
        # Pure mocks: compile independently and never call invalid native allocators.
        for name, sources in [
            ("runtime-unit", [ROOT / "tests/observation/runtime_ledger_tests.c"]),
            ("driver-unit", [ROOT / "tests/observation/test_driver.c", ROOT / "std/crypto/sha256_rt.c"]),
        ]:
            exe = self.artifacts / name
            self.command(name + "-build", [*shlex.split(cc), "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", *map(str, sources), "-o", str(exe)])
            self.command(name + "-run", [str(exe)])
        snapshots = []
        for opt in levels:
            suffix = f"-O{opt}"
            data = self.observed("coverage90" + suffix, FIXTURES / "coverage.sts", ["--min-test-coverage", "90"], opt,
                                 lines=COVERAGE_LINES, covered=9, assertions=(1, 0))
            assert data["uncovered_oracle"] == [14]
            snapshots.append((data["lines"], data["exclusions"]))
            self.observed("coverage91" + suffix, FIXTURES / "coverage.sts", ["--min-test-coverage", "91"], opt,
                          expected=1, lines=COVERAGE_LINES, covered=9)
            self.observed("balanced" + suffix, FIXTURES / "balanced.sts",
                          ["--warn-memleak", "--max-leaked-bytes", "0", "--min-test-coverage", "100"], opt,
                          lines=BALANCED_LINES, covered=5, byte_count=0, live=0, assertions=(1, 0))
            control = self.observed("controls" + suffix, FIXTURES / "control.sts",
                                    ["--max-leaked-bytes", "0", "--min-test-coverage", "0"], opt,
                                    lines=CONTROL_LINES, covered=28, byte_count=0, assertions=(2, 0), sentinel=True)
            assert control["uncovered_oracle"] == [17]
            assert control["exclusions"][37] == 5  # source line 38, after return
        assert all(item == snapshots[0] for item in snapshots), "optimizer changed source denominator/exclusions"
        self.observed("warning-only", FIXTURES / "leak.sts", ["--warn-memleak"], byte_count=8, live=1, lines=[])
        self.observed("leak-equality", FIXTURES / "leak.sts", ["--max-leaked-bytes", "8"], byte_count=8, live=1)
        self.observed("leak-over", FIXTURES / "leak.sts", ["--max-leaked-bytes", "7"], expected=1, byte_count=8, live=1)
        self.observed("leak-zero", FIXTURES / "leak.sts", ["--max-leaked-bytes", "0"], expected=1, byte_count=8, live=1)
        self.observed("zero-assertions", FIXTURES / "zero_assertions.sts", ["--min-test-coverage", "100"],
                      lines=[2], covered=1, assertions=(0, 0), sentinel=True)
        self.observed("zero-production", FIXTURES / "zero_lines.sts", ["--min-test-coverage", "0"], expected=1, lines=[], covered=0)
        self.observed("zero-production-leaks-only", FIXTURES / "zero_lines.sts", ["--max-leaked-bytes", "0"], lines=[], byte_count=0)
        eight = self.generated_source("eight-of-ten", (FIXTURES / "coverage.sts").read_text().replace("covered(true), 7", "covered(false), 6"))
        self.observed("eight-fails90", eight, ["--min-test-coverage", "90"], expected=1, lines=COVERAGE_LINES, covered=8)
        failed = self.generated_source("failed-assertion", (FIXTURES / "coverage.sts").read_text().replace("covered(true), 7", "covered(true), 8"))
        data = self.observed("assertion-fails", failed, ["--min-test-coverage", "90", "--max-leaked-bytes", "0"], expected=1,
                             lines=COVERAGE_LINES, covered=9, byte_count=0, assertions=(0, 1), child_exit=1)
        assert data["child"]["exit_code"] != 0
        named = self.generated_source("percent-name", "mod names;\ntest 'literal %s %n name' { expect.(true); }\n")
        self.observed("percent-name-safe", named, ["--max-leaked-bytes", "0"], assertions=(1, 0))
        cross = self.generated_source("cross-test", "mod cross;\nheap u8 *rw saved = nil;\ntest 'allocate' { saved = new.(8); }\ntest 'release' { rem.(saved); }\n")
        self.observed("suite-endpoint", cross, ["--max-leaked-bytes", "0"], byte_count=0, live=0, assertions=(0, 0), tests=2)
        boundary_name = "n" * 127
        boundary = self.generated_source("binding-boundary", f"mod boundary;\nfn {boundary_name}(void): i32 {{ ret 7; }}\ntest '127 byte binding' {{ expect_eq.({boundary_name}(), 7); }}\n")
        self.observed("binding-boundary", boundary, ["--min-test-coverage", "100"], lines=[2], covered=1)
        harmless = self.generated_source("harmless-symbol-text", "mod harmless;\n// __stasha_obs_init malloc imp macro\ntest 'strings are harmless' { print.('__stasha_obs_assert_pass __test_0'); expect.(true); }\n")
        self.observed("harmless-symbol-text", harmless, ["--max-leaked-bytes", "0"], assertions=(1, 0))
        expression = self.generated_source("expression-body", "mod expression;\nfn answer(void): i32 =>\n    42;\ntest 'expression body' { expect_eq.(answer(), 42); }\n")
        self.observed("expression-body", expression, ["--min-test-coverage", "100"], lines=[3], covered=1)
        same_line = self.generated_source("same-line", "mod same;\nfn work(void): i32 { stack i32 x = 1; x += 1; ret x; }\ntest 'one physical line' { expect_eq.(work(), 2); }\n")
        self.observed("same-line", same_line, ["--min-test-coverage", "100"], lines=[2], covered=1)
        excluded = self.generated_source("excluded-work", "mod excluded;\nfn work(void): i32 {\n    stack i32 _ = 123;\n    @comptime assert.(true && true);\n    comptime_if os == 'linux' { ret 1; } else { ret 2; }\n    ret 3;\n}\ntest 'compile-time and blank exclusions' { expect_eq.(work(), 1); }\n")
        data = self.observed("excluded-work", excluded, ["--min-test-coverage", "100"], lines=[5], covered=1)
        assert data["exclusions"][2:4] == [1, 1] and data["exclusions"][5] == 5
        extra_assertions = self.generated_source("extra-assertions", (FIXTURES / "coverage.sts").read_text() + "test 'more assertions' { expect.(true); expect.(true); }\n")
        self.observed("assertions-do-not-expand-production", extra_assertions, ["--min-test-coverage", "90"], lines=COVERAGE_LINES, covered=9, tests=2)
        for size in [1, 32, 4096]:
            source = self.generated_source(f"leak{size}", (FIXTURES / "leak.sts").read_text().replace("new.(8)", f"new.({size})"))
            self.observed(f"bytes{size}", source, ["--max-leaked-bytes", str(size)], byte_count=size, live=1)
        # A 179/200 ratio rounds to 90 at integer display precision but fails 90 exactly.
        body = "mod ratio;\nfn used(void): void {\n" + "    print.('');\n" * 179 + "}\nfn unused(void): void {\n" + "    print.('');\n" * 21 + "}\ntest 'ratio' { used(); }\n"
        ratio = self.generated_source("rounding", body)
        self.observed("rounding-fails", ratio, ["--min-test-coverage", "90"], expected=1, covered=179)
        # Unflagged compatibility: compile does not execute, then one explicit run.
        binary = self.artifacts / "unflagged.test"
        _, compiled = self.command("unflagged-build", [str(self.compiler), "test", str(FIXTURES / "zero_assertions.sts"), "-o", str(binary)])
        assert b"OBSERVATION_ONCE" not in compiled.stdout
        assert b"__stasha_obs_" not in binary.read_bytes(), "unflagged image depends on observation runtime"
        _, ran = self.command("unflagged-run", [str(binary)])
        assert ran.stdout.count(b"OBSERVATION_ONCE\n") == 1
        for name, declarations in REJECTIONS.items():
            source = self.generated_source("reject-" + name, "mod reject;\n" + declarations + "\ntest 'must not run' { print.('UNSUPPORTED_RAN\\n'); }\n")
            _, result = self.command("reject-" + name, [str(self.compiler), "test", str(source), "--warn-memleak", "-o", str(self.artifacts / (name + ".rejected"))], expected=1)
            assert b"UNSUPPORTED_RAN\n" not in result.stdout
            assert b"unsupported by observation profile" in result.stderr, (name, result.stderr)
        for name, source in [("zero-tests", FIXTURES / "zero_tests.sts"), ("raw-call-fixture", FIXTURES / "unsupported.sts")]:
            self.command("reject-" + name, [str(self.compiler), "test", str(source), "--warn-memleak", "-o", str(self.artifacts / (name + ".rejected"))], expected=1)
        for name, flags in {
            "negative-bytes": ["--max-leaked-bytes", "-1"], "overflow-bytes": ["--max-leaked-bytes", "18446744073709551616"],
            "space-bytes": ["--max-leaked-bytes", " 1"], "fraction-percent": ["--min-test-coverage", "90.5"],
            "over-percent": ["--min-test-coverage", "101"], "nan-percent": ["--min-test-coverage", "NaN"],
            "missing-percent": ["--min-test-coverage"], "malformed-repeated": ["--min-test-coverage", "bad", "--min-test-coverage", "0"],
        }.items():
            self.command("reject-option-" + name, [str(self.compiler), "test", str(FIXTURES / "coverage.sts"), *flags], expected=1)
        self.verify_inputs()
        self.status = "pass"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--opt", default="0,1,2,3")
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    levels = [int(level) for level in args.opt.split(",")]
    if not levels or any(level not in range(4) for level in levels): parser.error("--opt must list levels 0 through 3")
    qualification = Qualification(args.compiler, args.artifacts)
    try:
        qualification.run(levels, args.cc)
    except BaseException as error:
        qualification.status = "fail"
        qualification.failure = repr(error)
        raise
    finally:
        qualification.receipt()
    print(f"PASS: bounded native sequential observation profile; evidence {qualification.artifacts}")


if __name__ == "__main__":
    main()
