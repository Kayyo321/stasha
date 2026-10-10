#!/usr/bin/env python3
"""Mutation/selftests for the test runners, independent of a Stasha build."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
spec = importlib.util.spec_from_file_location("compiler_suites", HERE / "compiler_suites.py")
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)


class OracleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sandbox = tempfile.TemporaryDirectory(prefix="stasha-oracle-selftest-")
        cls.root = Path(cls.sandbox.name)
        # Test the real public entrypoints, safely away from tracked binaries.
        for suite in ["diag", "cinterop", "signals"]:
            target = cls.root / "tests" / suite
            target.mkdir(parents=True)
            runner = "run_tests.sh" if suite == "signals" else "run_" + suite + "_tests.sh"
            shutil.copy2(REPO / "tests" / suite / runner, target)
            for source in (REPO / "tests" / suite).rglob("*.sts"):
                dest = target / source.relative_to(REPO / "tests" / suite)
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, dest)
        (cls.root / "tests/harness").mkdir()
        shutil.copy2(HERE / "compiler_suites.py", cls.root / "tests/harness")
        cls.compiler = cls.root / "mock-stasha"
        shutil.copy2(HERE / "mock_compiler.py", cls.compiler)
        cls.compiler.chmod(0o755)
        build = cls.root / "tests/cinterop/build"
        build.mkdir()
        (build / "libcinterop_support.a").touch()

    @classmethod
    def tearDownClass(cls):
        cls.sandbox.cleanup()

    def invoke(self, mode="success", suite="cinterop", selector="^01_scalars$", extra=()):
        with tempfile.TemporaryDirectory(prefix="results-", dir=self.root) as artifacts:
            runner = "run_tests.sh" if suite == "signals" else "run_" + suite + "_tests.sh"
            command = ["bash", str(self.root / "tests" / suite / runner)]
            if suite != "signals":
                command.append(str(self.compiler))
            command += ["--filter", selector, "--artifacts", artifacts,
                       "--compile-timeout", "2", "--run-timeout", "1"] + list(extra)
            run = subprocess.run(command, cwd=self.root, env=dict(os.environ, STASHA_MOCK_MODE=mode, STASHA=str(self.compiler)),
                                 text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            files = list(Path(artifacts).glob("*/results.json"))
            summary = json.loads(files[0].read_text()) if files else None
        return run, summary

    def assert_failed(self, mode, message, **kwargs):
        run, summary = self.invoke(mode, **kwargs)
        self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
        self.assertIn(message, run.stdout)
        self.assertEqual(summary["counts"]["FAIL"], 1)
        return summary

    def test_positive_baseline(self):
        run, summary = self.invoke()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(summary["counts"]["PASS"], 1)
        self.assertEqual(summary["cases"][0]["compile"]["returncode"], 0)
        self.assertEqual(summary["cases"][0]["run"]["returncode"], 0)
        self.assertTrue(summary["compiler_sha256"])

    def test_broken_fixture_is_detected(self):
        self.assert_failed("fixturefailure", "expected exit 0 and stdout")

    def test_compiler_failure_even_with_fresh_executable(self):
        summary = self.assert_failed("compilerfailure", "compiler rejection (exit=1)")
        self.assertNotIn("run", summary["cases"][0])

    def test_stale_binary_is_never_run(self):
        stale = self.root / "tests/cinterop/build/01_scalars"
        marker = self.root / "STALE_WAS_RUN"
        stale.write_text("#!/bin/bash\ntouch '%s'\necho '01_scalars: ok'\n" % marker)
        stale.chmod(0o755)
        try:
            self.assert_failed("no-output", "fresh executable not produced")
            self.assertFalse(marker.exists(), "stale output executed")
            self.assertTrue(stale.exists(), "runner modified tracked output")
        finally:
            stale.unlink()

    def test_compiler_timeout(self):
        self.assert_failed("timeout", "compiler timeout", extra=["--compile-timeout", "0.1"])

    def test_runtime_timeout(self):
        self.assert_failed("runtime-timeout", "runtime timeout", extra=["--run-timeout", "0.1"])

    def test_runtime_crash(self):
        self.assert_failed("runtime-crash", "runtime crash")

    def test_runtime_nonzero_with_matching_output(self):
        self.assert_failed("runtime-failure", "runtime rejection (exit=1)")

    def test_success_text_on_stderr_is_not_stdout(self):
        self.assert_failed("stderr-only", "expected exit 0 and stdout")

    def test_diagnostic_pattern_has_no_stray_f(self):
        # Baseline has "undefined variable" exactly once, with no appended 'f'.
        run, summary = self.invoke(suite="diag", selector="^dedup_undefined_var$")
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(summary["cases"][0]["compile"]["returncode"], 1)
        self.assertEqual(summary["counts"]["PASS"], 1)

    def test_summary_regex_has_no_stray_f(self):
        run, _ = self.invoke(suite="diag", selector="^dedup_undefined_var/summary$")
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_negative_success_cannot_pass_on_matching_diagnostic(self):
        self.assert_failed("false-success", "compiler success (exit=0)", suite="diag",
                           selector="^dedup_undefined_var$")

    def test_negative_unexpected_exit_cannot_pass(self):
        self.assert_failed("wrong-exit", "compiler unexpected-exit (exit=2)", suite="diag",
                           selector="^dedup_undefined_var$")

    def test_negative_crash_cannot_pass_on_matching_diagnostic(self):
        self.assert_failed("crash", "compiler crash", suite="diag", selector="^dedup_undefined_var$")

    def test_negative_timeout_cannot_pass_on_matching_diagnostic(self):
        self.assert_failed("timeout", "compiler timeout", suite="diag", selector="^dedup_undefined_var$",
                           extra=["--compile-timeout", "0.1"])

    def test_negative_verifier_failure_cannot_pass(self):
        self.assert_failed("verifier", "compiler internal/verifier/sanitizer failure", suite="diag",
                           selector="^dedup_undefined_var$")

    def test_negative_sanitizer_failure_cannot_pass(self):
        self.assert_failed("sanitizer", "compiler internal/verifier/sanitizer failure", suite="diag",
                           selector="^dedup_undefined_var$")

    def test_negative_source_location_is_required(self):
        self.assert_failed("no-location", "missing source location", suite="diag",
                           selector="^dedup_undefined_var$")

    def test_negative_cannot_leave_output(self):
        self.assert_failed("output-on-rejection", "left an output artifact", suite="diag",
                           selector="^dedup_undefined_var$")

    def test_interop_negative_baseline(self):
        run, summary = self.invoke(selector="^neg/bitfield_unsupported$")
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(summary["counts"]["PASS"], 1)

    def test_interop_negative_crash_is_not_rejection(self):
        self.assert_failed("crash", "compiler crash", selector="^neg/bitfield_unsupported$")

    def test_warning_success_requires_zero_exit(self):
        self.assert_failed("compilerfailure", "compiler rejection", suite="diag",
                           selector="^warn_unused_with_wall$")

    def test_warning_success_baseline(self):
        run, _ = self.invoke(suite="diag", selector="^warn_unused_with_wall$")
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_zero_exit_with_error_diagnostic_fails(self):
        self.assert_failed("false-success-with-error", "emitted an error despite success status")

    def test_missing_fixture_fails(self):
        path = self.root / "tests/cinterop/01_scalars.sts"
        backup = path.with_suffix(".backup")
        path.rename(backup)
        try:
            self.assert_failed("success", "missing required fixture")
        finally:
            backup.rename(path)

    def test_llvm_skip_is_visible_and_can_be_required(self):
        run, summary = self.invoke(selector="^llvm_smoke$")
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(summary["counts"]["SKIP"], 1)
        self.assert_failed("success", "in-tree llvm-config unavailable", selector="^llvm_smoke$",
                           extra=["--require-prerequisites"])

    def test_empty_selection_is_not_a_green_suite(self):
        run, _ = self.invoke(selector="^does-not-exist$")
        self.assertEqual(run.returncode, 2)
        self.assertIn("selected no test cases", run.stderr)

    def test_missing_compiler_is_launch_error(self):
        compiler = self.compiler
        self.compiler = self.root / "does-not-exist"
        try:
            self.assert_failed("success", "compiler launch-error")
        finally:
            self.compiler = compiler

    def test_capture_preserves_status_and_command(self):
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            result = harness.capture([sys.executable, "-c", "import sys; sys.exit(37)"], self.root,
                                     Path(directory) / "process", 1)
            self.assertEqual(result["returncode"], 37)
            self.assertEqual(result["category"], "unexpected-exit")
            saved = json.loads((Path(directory) / "process.json").read_text())
            self.assertEqual(saved["command"], result["command"])

    def test_signals_exact_output_and_nonzero_exit(self):
        for name in ["basic", "quit_from_exit"]:
            run, summary = self.invoke(suite="signals", selector="^" + name + "$")
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(summary["counts"]["PASS"], 1)
        self.assertEqual(summary["cases"][0]["run"]["returncode"], 1)

    def test_signals_extra_output_is_failure(self):
        self.assert_failed("extra-output", "expected exit 0 and exact stdout", suite="signals", selector="^basic$")

    def test_signals_runtime_timeout(self):
        self.assert_failed("runtime-timeout", "runtime timeout", suite="signals", selector="^basic$",
                           extra=["--run-timeout", "0.1"])

    @unittest.skipUnless(os.name == "posix", "POSIX process group semantics")
    def test_timeout_kills_descendants(self):
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            marker = Path(directory) / "CHILD_SURVIVED"
            child = "import time; from pathlib import Path; time.sleep(0.3); Path(%r).touch()" % str(marker)
            parent = "import subprocess, sys, time; subprocess.Popen([sys.executable, '-c', %r]); time.sleep(10)" % child
            result = harness.capture([sys.executable, "-c", parent], self.root, Path(directory) / "process", 0.1)
            self.assertEqual(result["category"], "timeout")
            time.sleep(0.4)
            self.assertFalse(marker.exists(), "timed-out process leaked a child")

    def test_xfail_is_narrow_and_xpass_fails(self):
        case = harness.interop_cases()[-1]
        args = argparse.Namespace(support_lib=self.root / "tests/cinterop/build/libcinterop_support.a",
                                  compiler=str(self.compiler), compile_timeout=1, run_timeout=1)
        compiled = dict(category="success", returncode=0, stderr="", stdout="")

        def trial(runtime):
            with tempfile.TemporaryDirectory(dir=self.root) as directory:
                def fake_capture(command, cwd, prefix, timeout, extra_env=None):
                    if "build" in command:
                        output = Path(command[command.index("-o") + 1])
                        output.write_text("#!/bin/bash\nexit 0\n")
                        output.chmod(0o755)
                        return compiled
                    return dict(runtime)
                with patch.object(harness, "capture", side_effect=fake_capture), \
                        patch.object(harness.platform, "system", return_value="Darwin"), \
                        patch.object(harness.platform, "machine", return_value="arm64"):
                    return harness.run_case(case, args, Path(directory))

        known = dict(category="rejection", returncode=1, stderr="",
                     stdout="XFAIL struct-byval arg got wrong value\n")
        self.assertEqual(trial(known)["status"], "XFAIL")
        self.assertEqual(trial(dict(known, category="crash", returncode=-9))["status"], "FAIL")
        self.assertEqual(trial(dict(known, category="timeout", returncode=-9))["status"], "FAIL")
        self.assertEqual(trial(dict(known, stdout="unrelated error"))["status"], "FAIL")
        self.assertEqual(trial(dict(known, stdout=known["stdout"] + "unrelated error"))["status"], "FAIL")
        self.assertEqual(trial(dict(known, stderr="unrelated error"))["status"], "FAIL")
        self.assertEqual(trial(dict(known, category="success", returncode=0,
                                    stdout="04b_struct_byval_arg: ok"))["status"], "XPASS")


if __name__ == "__main__":
    unittest.main(verbosity=2)
