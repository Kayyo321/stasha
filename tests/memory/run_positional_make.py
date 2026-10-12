#!/usr/bin/env python3
"""Frozen ordinary positional-slice positives, shared by the fifth-family runner.

The command-line modes are read-only inventory and dependency-free selftests.
Native qualification is dispatched by tests/observation/test_positional_slices.py
through its FamilySession; this module cannot issue an ordinary-only fifth-family
PASS receipt. Rejections, resource controls, OOM and malformed programs are never
materialized or dispatched by this runner. Ordinary allocation elision is allowed.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys

if not __debug__:
    raise RuntimeError("qualification requires Python assertions enabled")
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
CORPUS = ROOT / "tests/observation/fixtures/positional_slices/corpus.json"
SOURCE_PREFIX = Path("sources/positional-slices")
R2_CORPUS_SHA256 = "9fa7898fe653f8b8536b7dd8391497033072a30e7484dc48ab88b4b8f774b323"
# Derived from the independently reviewed, immutable R2 positive-case data,
# never from compiler output. This pin includes source bytes AND literal oracles.
POSITIVE_CASES_SHA256 = "29a044746e1ee6d9bfbaa618676b7113134e81c82f5b6134c87f8986fba34526"
NATIVE_NAMES = (
    "coverage", "retained", "primitive_i8", "primitive_u8", "primitive_i16",
    "primitive_u16", "primitive_i32", "primitive_u32", "primitive_i64",
    "primitive_u64", "primitive_bool", "primitive_f32", "primitive_f64",
    "conversions", "empty_and_single", "scalar_reads", "scalar_operators",
    "scalar_comparisons", "helper_order", "helper_explicit_heap", "plain_multiline",
    "conditional_items", "short_circuit_items", "shadow_initializer", "same_line",
    "imports", "test_owned", "schedule_direct", "schedule_defer_fallthrough",
    "schedule_defer_return", "schedule_defer_nested", "schedule_defer_continue",
    "schedule_defer_break", "schedule_defer_nested_exits", "schedule_direct_bypass_return",
    "schedule_direct_bypass_break", "schedule_direct_bypass_continue",
    "schedule_shadow_siblings", "mixed_none", "mixed_explicit_rem", "mixed_count_rem",
    "mixed_positional_rem", "mixed_all_rem", "promoted_inline_make-called",
    "promoted_inline_make-dead", "promoted_inline_make-uncalled",
)
LEVELS = (0, 1, 2, 3)
# The frozen promotions contain unused locals. Ordinary compiler warnings are
# permitted; only the executed test image has an exact empty-stderr contract.
COMPILE_FAILURES = re.compile(
    rb"\b(?:error|fatal|sanitizer)\b|llvm[^\r\n]*(?:verif|invalid)|"
    rb"verification[^\r\n]*fail|code[ _-]*gen(?:eration)?[^\r\n]*fail|"
    rb"optimi[sz](?:ation|er)[^\r\n]*fail|assert(?:ion)?[^\r\n]*(?:fail|abort)|"
    rb"segmentation fault|stack dump|broken (?:module|function)|invalid (?:debug|!dbg)",
    re.IGNORECASE,
)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        assert key not in result, "duplicate JSON member: " + key
        result[key] = value
    return result


def _relative(value):
    assert isinstance(value, str) and value and "\\" not in value
    path = PurePosixPath(value)
    assert not path.is_absolute() and ".." not in path.parts and "." not in path.parts
    assert str(path) == value, "noncanonical source path"
    return Path(value)


def _regular(path, *, executable=False):
    path = Path(path)
    assert path.resolve() == path.absolute(), "artifact path rebound: " + str(path)
    info = path.lstat()
    assert stat.S_ISREG(info.st_mode) and info.st_size > 0, "missing/empty regular artifact: " + str(path)
    if executable:
        assert os.access(path, os.X_OK), "image is not executable: " + str(path)
    return path


def load_corpus(path=None):
    """Load only frozen positive semantics; observation metadata is not consulted."""
    path = CORPUS if path is None else Path(path)
    document = json.loads(_regular(path).read_text(encoding="utf-8"), object_pairs_hook=_unique_object)
    corpus = document["r2_corpus"]
    cases = corpus["positive_cases"]
    encoded = json.dumps(cases, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    assert hashlib.sha256(encoded).hexdigest() == POSITIVE_CASES_SHA256, "frozen positive corpus changed"
    assert tuple(case["id"] for case in cases) == NATIVE_NAMES and len(cases) == 46
    paths = set()
    for case in cases:
        assert case["partition"] == "ordinary_and_observed_native_positive"
        ordinary = case["ordinary"]
        assert ordinary["opt_levels"] == list(LEVELS)
        assert type(ordinary["exit"]) is int and ordinary["exit"] == 0 and ordinary["stderr"] == ""
        assert isinstance(ordinary["stdout"], str)
        assert type(ordinary["tests"]) is int and ordinary["tests"] > 0
        assert type(ordinary["assertions"]) is int and ordinary["assertions"] >= 0
        relative_paths = set()
        for source in case["sources"]:
            relative = _relative(source["relative_path"])
            path_value = _relative(source["path"])
            assert path_value == Path("native") / case["id"] / relative
            assert path_value not in paths and relative not in relative_paths
            paths.add(path_value); relative_paths.add(relative)
            raw = source["text"].encode("ascii")
            assert raw and b"\0" not in raw
            assert hashlib.sha256(raw).hexdigest() == source["sha256"], "source text/hash mismatch"
        assert _relative(case["entry"]) in relative_paths
    assert len(paths) == 47, "positive imported source inventory changed"
    return corpus


def positive_cases():
    """Return fresh dictionaries so callers cannot mutate the pinned loader state."""
    return tuple(load_corpus()["positive_cases"])


def require_full_levels(levels):
    assert isinstance(levels, (list, tuple)) and all(type(opt) is int for opt in levels)
    assert tuple(levels) == LEVELS, "positional native qualification requires --opt 0,1,2,3"


def expected_cases(levels):
    require_full_levels(levels)
    return {"ordinary_native": [f"{name}-ordinary-O{opt}" for opt in levels for name in NATIVE_NAMES]}


def _source_path(session, source):
    return session.artifacts / SOURCE_PREFIX / _relative(source["path"])


def _entry_path(session, case):
    return next(_source_path(session, source) for source in case["sources"]
                if source["relative_path"] == case["entry"])


def materialize(session):
    """Write the 47 positive sources once into this fresh evidence session."""
    cases = positive_cases()
    base = session.artifacts / SOURCE_PREFIX / "native"
    assert base.resolve() == base.absolute(), "source destination rebound"
    base.mkdir(parents=True, exist_ok=False)
    for case in cases:
        for source in case["sources"]:
            path = _source_path(session, source)
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("xb") as output:
                output.write(source["text"].encode("ascii"))
            assert sha(_regular(path)) == source["sha256"]
    sources = {case["id"]: _entry_path(session, case) for case in cases}
    session.freeze_generated()
    validate_positive_sources(session, sources)
    return sources


def validate_positive_sources(session, sources):
    """Validate all root/imported files before dispatch, including path identity."""
    assert tuple(sources) == NATIVE_NAMES, "positive dispatch selection changed"
    cases = positive_cases()
    expected = set()
    for case in cases:
        assert sources[case["id"]] == _entry_path(session, case), "positive entry substituted"
        for source in case["sources"]:
            path = _source_path(session, source)
            assert sha(_regular(path)) == source["sha256"], "positive source changed: " + source["path"]
            expected.add(path)
    assert set((session.artifacts / SOURCE_PREFIX / "native").rglob("*.sts")) == expected, "positive source inventory changed"
    session.verify_inputs()


def assert_output(result, oracle):
    """Exact frozen bytes and normal exit; PASS counts alone are insufficient."""
    assert result.returncode == oracle["exit"] == 0, "ordinary image did not exit normally with zero"
    assert result.stdout == oracle["stdout"].encode("ascii"), ("ordinary stdout mismatch", result.stdout)
    assert result.stderr == oracle["stderr"].encode("ascii") == b"", ("ordinary stderr mismatch", result.stderr)


def _artifact_hashes(binary, pre, post):
    """No observer hooks in either ordinary IR phase or the exact native image.

    Deliberately do not count malloc/free: ordinary as-if optimization may remove
    allocation pairs. Failure-dominance and source lowering have separate source
    and nonlaunch IR qualification; stdout is never an allocation-event oracle.
    """
    paths = ((_regular(binary, executable=True), "binary_sha256"),
             (_regular(pre), "pre_ir_sha256"), (_regular(post), "post_ir_sha256"))
    hashes = {}
    for path, name in paths:
        content = path.read_bytes()
        assert b"__stasha_obs_" not in content, "observer hook in ordinary artifact: " + str(path)
        if path != binary:
            assert re.search(rb"^define\b", content, re.MULTILINE), "ordinary IR lacks a definition"
            assert re.search(rb"^define\b[^\n]*@main\(", content, re.MULTILINE), "ordinary IR lacks test main"
        hashes[name] = hashlib.sha256(content).hexdigest()
    return hashes


def _case_paths(session, key):
    return tuple(session.artifacts / (key + suffix) for suffix in (".test", ".pre.ll", ".post.ll"))


def validate_ordinary_receipts(session, sources, levels):
    """The complete ordinary barrier, revalidated before any observed launch."""
    require_full_levels(levels)
    validate_positive_sources(session, sources)
    cases = {case["id"]: case for case in positive_cases()}
    required = expected_cases(levels)["ordinary_native"]
    receipts = [case for case in session.cases if case["stage"] == "ordinary_native"]
    assert len(receipts) == len(required) == 184
    assert [case["name"] for case in receipts] == required, "ordinary receipt inventory/order changed"
    records = {record["name"]: record for record in session.records}
    assert len(records) == len(session.records), "duplicate command record"
    for receipt in receipts:
        key = receipt["name"]
        name = key.rsplit("-ordinary-O", 1)[0]
        case = cases[name]
        assert receipt["status"] == "pass" and type(receipt["executions"]) is int and receipt["executions"] == 1
        paths = _case_paths(session, key)
        for path, field in zip(paths, ("binary_path", "pre_ir_path", "post_ir_path")):
            assert receipt[field] == str(path.relative_to(session.artifacts)), "ordinary artifact identity changed"
        hashes = _artifact_hashes(*paths)
        assert all(receipt[field] == digest for field, digest in hashes.items()), "ordinary artifact changed after execution"
        assert receipt["source_sha256"] == sha(sources[name])
        assert receipt["source_hashes"] == {source["path"]: source["sha256"] for source in case["sources"]}
        assert receipt["tests"] == case["ordinary"]["tests"] and receipt["assertions_passed"] == case["ordinary"]["assertions"]
        for suffix in ("-compile", "-execute"):
            record = records[key + suffix]
            assert record["status"] == "pass" and record["returncode"] == record["expected"] == 0
        assert records[key + "-execute"]["command"] == [str(paths[0])], "ordinary image dispatch changed"
        opt = int(key[-1])
        assert records[key + "-compile"]["command"] == list(map(str, (
            session.compiler, "test", sources[name], f"-o={opt}", "-o", paths[0]))), "ordinary compile dispatch changed"
    session.verify_inputs()


def run_ordinary(session, sources, levels):
    """Compile only, audit both IR phases, then execute each exact image once."""
    require_full_levels(levels)
    validate_positive_sources(session, sources)
    assert not any(case["stage"] in ("ordinary_native", "observed_native") for case in session.cases), "native phase already started"
    cases = positive_cases()
    for opt in levels:
        for case in cases:
            validate_positive_sources(session, sources)
            name = case["id"]; key = f"{name}-ordinary-O{opt}"
            binary, pre, post = _case_paths(session, key)
            for path in (binary, pre, post):
                assert not os.path.lexists(path), "stale ordinary output: " + str(path)
            env = dict(os.environ, STS_DUMP_IR_PRE=str(pre), STS_DUMP_IR=str(post))
            _, compiled = session.command(key + "-compile", [session.compiler, "test", sources[name],
                f"-o={opt}", "-o", binary], expected=0, env=env)
            assert compiled.returncode == 0, "ordinary compilation failed"
            assert not COMPILE_FAILURES.search(compiled.stdout + b"\n" + compiled.stderr), "ordinary compiler diagnostic failure"
            assert b"PASS\n" not in compiled.stdout and b"passed, " not in compiled.stdout, "compiler unexpectedly ran ordinary tests"
            hashes = _artifact_hashes(binary, pre, post)
            validate_positive_sources(session, sources)
            assert _artifact_hashes(binary, pre, post) == hashes, "ordinary artifact changed before execution"
            _, result = session.command(key + "-execute", [binary], expected=0)
            assert_output(result, case["ordinary"])
            assert _artifact_hashes(binary, pre, post) == hashes, "ordinary artifact changed during execution"
            validate_positive_sources(session, sources)
            session.cases.append(dict(name=key, stage="ordinary_native", status="pass", executions=1,
                **hashes, binary_path=str(binary.relative_to(session.artifacts)),
                pre_ir_path=str(pre.relative_to(session.artifacts)), post_ir_path=str(post.relative_to(session.artifacts)),
                source_sha256=sha(sources[name]), source_hashes={source["path"]: source["sha256"] for source in case["sources"]},
                assertions_passed=case["ordinary"]["assertions"], tests=case["ordinary"]["tests"]))
    validate_ordinary_receipts(session, sources, levels)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--list", action="store_true", help="show the 46 frozen positives without compilation")
    mode.add_argument("--selftest-only", action="store_true", help="run Python/mocks only; no native tools")
    args = parser.parse_args(argv)
    if args.list:
        print(json.dumps([{"id": case["id"], "source_hashes": {source["path"]: source["sha256"] for source in case["sources"]},
                           "ordinary": case["ordinary"]} for case in positive_cases()], indent=2))
        return 0
    spec = importlib.util.spec_from_file_location("positional_make_selftests", Path(__file__).with_name("test_positional_make.py"))
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    import unittest
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromModule(module))
    return int(not result.wasSuccessful())


if __name__ == "__main__":
    raise SystemExit(main())
