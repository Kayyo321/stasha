#!/usr/bin/env python3
"""Dependency-free pure controls for the positional ordinary-before-observed gate.

Every compiler and generated-image command is mocked. A process-launch tripwire
is installed for each test. Fake images are inert bytes and are never executed.
"""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("positional_ordinary", Path(__file__).with_name("run_positional_make.py"))
ordinary = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ordinary)

# Allocation-free IR is deliberate: the ordinary as-if rule may eliminate all
# malloc/free calls. It must still have a main definition and no observer hooks.
MOCK_IR = b"define i32 @main() {\nentry:\n  ret i32 0\n}\n"
MOCK_IMAGE = b"INERT MOCK IMAGE; NOT A NATIVE EXECUTABLE\n"


class MockSession:
    """FamilySession-shaped recorder; never calls subprocess or another runner."""
    def __init__(self, artifacts, defect=None):
        self.artifacts = Path(artifacts)
        self.compiler = self.artifacts / "MOCK-COMPILER-NEVER-RUN"
        self.compiler.write_bytes(b"not executable")
        self.compiler_hash = ordinary.sha(self.compiler)
        self.records, self.cases = [], []
        self.generated = {}
        self.resolutions = {}
        self.defect = defect
        self.compilations = self.executions = 0
        self.oracles = {case["id"]: case["ordinary"] for case in ordinary.positive_cases()}

    def freeze_generated(self):
        self.generated = {str(path.relative_to(self.artifacts)): ordinary.sha(path)
                          for path in (self.artifacts / "sources").rglob("*.sts")}
        self.resolutions = {name: (self.artifacts / name).resolve() for name in self.generated}

    def verify_inputs(self):
        assert ordinary.sha(self.compiler) == self.compiler_hash
        for name, digest in self.generated.items():
            path = self.artifacts / name
            assert ordinary.sha(path) == digest
            assert path.resolve() == self.resolutions[name]
        assert {str(path.relative_to(self.artifacts)) for path in (self.artifacts / "sources").rglob("*.sts")} == set(self.generated)

    def command(self, name, argv, *, expected=0, env=None):
        argv = list(map(str, argv))
        record = dict(name=name, command=argv, expected=expected, status="running")
        self.records.append(record)
        self.verify_inputs()
        is_compile = name.endswith("-compile")
        if is_compile:
            self.compilations += 1
            assert argv[0] == str(self.compiler) and argv[1] == "test" and argv[-2] == "-o"
            assert not any(arg.startswith("--warn-") or arg.startswith("--max-") or arg.startswith("--min-") for arg in argv)
            binary = Path(argv[-1])
            binary.write_bytes(MOCK_IMAGE); binary.chmod(0o700)
            pre, post = Path(env["STS_DUMP_IR_PRE"]), Path(env["STS_DUMP_IR"])
            pre.write_bytes(MOCK_IR); post.write_bytes(MOCK_IR)
            result = subprocess.CompletedProcess(argv, 0, b"", b"")
        else:
            self.executions += 1
            assert name.endswith("-execute") and len(argv) == 1
            case_id = name.rsplit("-ordinary-O", 1)[0]
            result = subprocess.CompletedProcess(argv, 0, self.oracles[case_id]["stdout"].encode("ascii"), b"")
        if self.defect:
            self.defect(self, name, argv, env, result)
        record.update(returncode=result.returncode, status="pass" if result.returncode == expected else "fail")
        # Intentionally do not enforce result.exit here. The ordinary runner
        # must be fail-closed even with a deficient recorder mock.
        return self.artifacts / name, result


class PositionalMakeTests(unittest.TestCase):
    def setUp(self):
        self.no_process = mock.patch.object(subprocess, "run", side_effect=AssertionError("pure tests cannot launch processes"))
        self.no_process.start(); self.addCleanup(self.no_process.stop)
        self.temp = tempfile.TemporaryDirectory(prefix="positional-pure-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def session(self, defect=None):
        session = MockSession(self.root, defect)
        sources = ordinary.materialize(session)
        return session, sources

    def write_corpus(self, cases):
        path = self.root / "corpus.json"
        path.write_text(json.dumps({"r2_corpus": {"positive_cases": cases}}))
        return path

    def test_frozen_inventory_and_independent_source_hashes(self):
        cases = ordinary.positive_cases()
        self.assertEqual(len(cases), 46)
        self.assertEqual(tuple(case["id"] for case in cases), ordinary.NATIVE_NAMES)
        self.assertEqual(sum(len(case["sources"]) for case in cases), 47)
        self.assertEqual(len(ordinary.expected_cases([0, 1, 2, 3])["ordinary_native"]), 184)
        for case in cases:
            self.assertEqual(case["ordinary"]["exit"], 0)
            self.assertEqual(case["ordinary"]["stderr"], "")
            for source in case["sources"]:
                self.assertEqual(hashlib.sha256(source["text"].encode("ascii")).hexdigest(), source["sha256"])
        promotions = [case for case in cases if case["id"].startswith("promoted_")]
        self.assertEqual(len(promotions), 3)
        self.assertTrue(all(case["ordinary"]["assertions"] == 0 for case in promotions))
        self.assertEqual(next(case for case in cases if case["id"] == "imports")["ordinary"]["tests"], 2)

    def test_loader_needs_no_observation_oracle(self):
        cases = ordinary.positive_cases()
        self.assertEqual(ordinary.load_corpus(self.write_corpus(cases))["positive_cases"], list(cases))

    def test_corrupt_positive_oracle_or_source_cannot_be_refrozen(self):
        original = list(ordinary.positive_cases())
        mutations = (
            lambda cases: cases.pop(),
            lambda cases: cases.reverse(),
            lambda cases: cases[0]["ordinary"].update(stdout="learned from implementation\n"),
            lambda cases: cases[0]["ordinary"].update(exit=1),
            lambda cases: cases[0]["sources"][0].update(path="../escape.sts"),
            lambda cases: cases[0]["sources"][0].update(text="mod changed;\n", sha256=hashlib.sha256(b"mod changed;\n").hexdigest()),
            lambda cases: cases[0].update(partition="nonlaunch_rejection"),
        )
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                cases = copy.deepcopy(original); mutate(cases)
                with self.assertRaises(AssertionError):
                    ordinary.load_corpus(self.write_corpus(cases))

    def test_duplicate_json_member_rejected(self):
        path = self.write_corpus(ordinary.positive_cases())
        path.write_text(path.read_text().replace('{"r2_corpus":', '{"r2_corpus": null, "r2_corpus":', 1))
        with self.assertRaisesRegex(AssertionError, "duplicate JSON"):
            ordinary.load_corpus(path)

    def test_materialization_exact_and_fresh(self):
        session, sources = self.session()
        self.assertEqual(tuple(sources), ordinary.NATIVE_NAMES)
        self.assertEqual(len(session.generated), 47)
        self.assertTrue(all(name.startswith("sources/positional-slices/native/") for name in session.generated))
        self.assertEqual(sources["imports"], self.root / "sources/positional-slices/native/imports/root.sts")
        ordinary.validate_positive_sources(session, sources)
        with self.assertRaises(FileExistsError):
            ordinary.materialize(session)
        self.assertEqual(session.records, [])

    def test_source_changes_and_substitution_block_dispatch(self):
        session, sources = self.session()
        entry = sources["coverage"]; original = entry.read_bytes()
        entry.write_bytes(original + b"\n")
        with self.assertRaises(AssertionError):
            ordinary.run_ordinary(session, sources, [0, 1, 2, 3])
        entry.write_bytes(original)
        alternate = dict(sources); alternate["coverage"] = sources["retained"]
        with self.assertRaises(AssertionError):
            ordinary.run_ordinary(session, alternate, [0, 1, 2, 3])
        leaf = sources["imports"].parent / "util/leaf.sts"
        leaf.write_bytes(leaf.read_bytes() + b"\n")
        with self.assertRaises(AssertionError):
            ordinary.run_ordinary(session, sources, [0, 1, 2, 3])
        self.assertEqual(session.records, [])

    def test_extra_missing_and_rebound_sources_are_rejected(self):
        session, sources = self.session()
        entry = sources["coverage"]; content = entry.read_bytes()
        extra = entry.parent / "unexpected.sts"; extra.write_bytes(b"mod unexpected;\n")
        with self.assertRaises(AssertionError):
            ordinary.validate_positive_sources(session, sources)
        extra.unlink(); entry.unlink()
        with self.assertRaises(FileNotFoundError):
            ordinary.validate_positive_sources(session, sources)
        target = self.root / "same-bytes.sts"; target.write_bytes(content); entry.symlink_to(target)
        with self.assertRaises(AssertionError):
            ordinary.validate_positive_sources(session, sources)
        self.assertEqual(session.records, [])

    def test_only_exact_full_optimization_order_is_native(self):
        session, sources = self.session()
        for levels in ([], [0], [0, 1, 2], [3, 2, 1, 0], [0, 1, 2, 3, 3], [False, 1, 2, 3], [0, 1, 2, 4]):
            with self.subTest(levels=levels), self.assertRaises(AssertionError):
                ordinary.run_ordinary(session, sources, levels)
        self.assertEqual(session.records, [])

    def test_output_is_exact_bytes_and_normal_exit(self):
        oracle = ordinary.positive_cases()[0]["ordinary"]
        correct = subprocess.CompletedProcess([], 0, oracle["stdout"].encode("ascii"), b"")
        ordinary.assert_output(correct, oracle)
        for changes in ({"stdout": correct.stdout + b"extra\n"}, {"stdout": correct.stdout.replace(b"coverage", b"different")},
                        {"stdout": correct.stdout.replace(b"\n", b"\r\n")}, {"stderr": b"warning\n"},
                        {"returncode": 1}, {"returncode": -11}):
            result = copy.copy(correct)
            for field, value in changes.items(): setattr(result, field, value)
            with self.subTest(changes=changes), self.assertRaises(AssertionError):
                ordinary.assert_output(result, oracle)

    def test_compile_failures_never_execute_or_observe(self):
        for defect_name in ("exit", "timeout", "diagnostic-error", "verifier", "ran-tests", "missing-image", "empty-image", "not-executable",
                            "missing-pre", "missing-post", "bad-ir", "observer-image", "observer-pre", "observer-post", "symlink-image", "source-change"):
            with self.subTest(defect=defect_name), tempfile.TemporaryDirectory(dir=self.root) as tmp:
                def defect(session, name, argv, env, result):
                    if not name.endswith("-compile"): return
                    image = Path(argv[-1]); pre = Path(env["STS_DUMP_IR_PRE"]); post = Path(env["STS_DUMP_IR"])
                    if defect_name == "exit": result.returncode = 1
                    elif defect_name == "timeout": raise subprocess.TimeoutExpired(argv, 120)
                    elif defect_name == "diagnostic-error": result.stderr = b"error: unwanted\n"
                    elif defect_name == "verifier": result.stderr = b"LLVM IR verification failed\n"
                    elif defect_name == "ran-tests": result.stdout = b"test 'case' ... PASS\n\n1 passed, 0 failed\n"
                    elif defect_name == "missing-image": image.unlink()
                    elif defect_name == "empty-image": image.write_bytes(b"")
                    elif defect_name == "not-executable": image.chmod(0o600)
                    elif defect_name == "missing-pre": pre.unlink()
                    elif defect_name == "missing-post": post.unlink()
                    elif defect_name == "bad-ir": post.write_bytes(b"not LLVM IR\n")
                    elif defect_name == "observer-image": image.write_bytes(MOCK_IMAGE + b"__stasha_obs_init_v4")
                    elif defect_name == "observer-pre": pre.write_bytes(MOCK_IR + b"; __stasha_obs_slice_new_v4\n")
                    elif defect_name == "observer-post": post.write_bytes(MOCK_IR + b"; __stasha_obs_slice_new_v4\n")
                    elif defect_name == "symlink-image":
                        other = image.with_suffix(".other"); image.rename(other); image.symlink_to(other)
                    elif defect_name == "source-change":
                        source = Path(argv[2]); source.write_bytes(source.read_bytes() + b"\n")
                session = MockSession(Path(tmp), defect); sources = ordinary.materialize(session)
                observe = mock.Mock()
                with self.assertRaises((AssertionError, OSError, subprocess.TimeoutExpired)):
                    ordinary.run_ordinary(session, sources, [0, 1, 2, 3]); observe()
                self.assertEqual(session.executions, 0); self.assertEqual(session.cases, [])
                observe.assert_not_called()

    def test_native_failures_block_observation_and_pass_receipt(self):
        for defect_name in ("stdout", "stderr", "exit", "signal", "timeout", "changed-image", "missing-image", "changed-pre", "source-change", "import-change"):
            with self.subTest(defect=defect_name), tempfile.TemporaryDirectory(dir=self.root) as tmp:
                def defect(session, name, argv, env, result):
                    if not name.endswith("-execute"): return
                    image = Path(argv[0])
                    if defect_name == "stdout": result.stdout += b"unexpected\n"
                    elif defect_name == "stderr": result.stderr = b"unexpected\n"
                    elif defect_name == "exit": result.returncode = 1
                    elif defect_name == "signal": result.returncode = -11
                    elif defect_name == "timeout": raise subprocess.TimeoutExpired(argv, 120)
                    elif defect_name == "changed-image": image.write_bytes(MOCK_IMAGE + b"changed\n")
                    elif defect_name == "missing-image": image.unlink()
                    elif defect_name == "changed-pre": image.with_suffix(".pre.ll").write_bytes(MOCK_IR + b"; changed\n")
                    else:
                        relative = "coverage/root.sts" if defect_name == "source-change" else "imports/util/leaf.sts"
                        source = session.artifacts / ordinary.SOURCE_PREFIX / "native" / relative
                        source.write_bytes(source.read_bytes() + b"\n")
                session = MockSession(Path(tmp), defect); sources = ordinary.materialize(session)
                observe = mock.Mock()
                with self.assertRaises((AssertionError, OSError, subprocess.TimeoutExpired)):
                    ordinary.run_ordinary(session, sources, [0, 1, 2, 3]); observe()
                self.assertEqual(session.executions, 1); self.assertEqual(session.cases, [])
                observe.assert_not_called()

    def test_stale_artifact_and_repeat_execution_rejected(self):
        session, sources = self.session()
        stale = self.root / "coverage-ordinary-O0.test"; stale.write_bytes(MOCK_IMAGE)
        with self.assertRaisesRegex(AssertionError, "stale ordinary"):
            ordinary.run_ordinary(session, sources, [0, 1, 2, 3])
        self.assertEqual(session.records, [])
        stale.unlink(); session.cases.append(dict(name="earlier", stage="observed_native"))
        with self.assertRaisesRegex(AssertionError, "native phase already started"):
            ordinary.run_ordinary(session, sources, [0, 1, 2, 3])
        self.assertEqual(session.records, [])

    def test_full_mock_matrix_and_postphase_tamper_barrier(self):
        warnings = []
        def benign_unused_warning(session, name, argv, env, result):
            if name.startswith("promoted_inline_make-") and name.endswith("-compile"):
                result.stderr = (argv[2] + ":2: warning: unused variable 's'\n").encode("ascii")
                warnings.append(name)
        session, sources = self.session(benign_unused_warning)
        ordinary.run_ordinary(session, sources, [0, 1, 2, 3])
        self.assertEqual(len(warnings), 12, "benign compiler warnings must not change the frozen positives")
        self.assertEqual((session.compilations, session.executions, len(session.records)), (184, 184, 368))
        self.assertEqual([case["name"] for case in session.cases], ordinary.expected_cases([0, 1, 2, 3])["ordinary_native"])
        self.assertTrue(all(case["status"] == "pass" and case["executions"] == 1 for case in session.cases))
        ordinary.validate_ordinary_receipts(session, sources, [0, 1, 2, 3])
        saved = copy.deepcopy(session.cases)
        for mutation in (lambda: session.cases.pop(), lambda: session.cases.append(copy.deepcopy(session.cases[0])),
                         lambda: session.cases[0].update(status="fail"), lambda: session.cases[0].update(executions=2),
                         lambda: session.cases[0].update(binary_path="elsewhere")):
            with self.subTest(mutation=mutation):
                mutation()
                with self.assertRaises(AssertionError):
                    ordinary.validate_ordinary_receipts(session, sources, [0, 1, 2, 3])
                session.cases = copy.deepcopy(saved)
        image = self.root / session.cases[0]["binary_path"]
        image.write_bytes(MOCK_IMAGE + b"changed after the full matrix\n")
        observe = mock.Mock()
        with self.assertRaises(AssertionError):
            ordinary.validate_ordinary_receipts(session, sources, [0, 1, 2, 3]); observe()
        observe.assert_not_called()
        image.unlink()
        with self.assertRaises(OSError):
            ordinary.validate_ordinary_receipts(session, sources, [0, 1, 2, 3])
        self.assertEqual(session.executions, 184)


if __name__ == "__main__":
    unittest.main()
