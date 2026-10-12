#!/usr/bin/env python3
"""Bounded observed fixed-primitive-array query qualification.

Preparation and selftests never invoke a compiler. Every negative and every
source/IR-only control requires a separately reviewed fork-disabled compiler.
Native stages require prior independent source and pre/post-IR acceptance.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

if not __debug__:
    raise RuntimeError("qualification requires Python assertions enabled")
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests/observation/fixtures/array_queries"
FAMILY = "observed-array-queries"
POLICY = "closed-direct-source-array-and-local-primitive-slices-v1"
ORDINARY_NAMES = ("direct", "named_extent", "inferred_extent", "empty_extent", "empty_inferred",
                  "const_array", "final_array", "matrix", "deep_rank", "global_array")
FIXED_NAMES = ("coverage80", "coverage81", "coverage-only", "allocation-only", "control-anchors",
               "retained13", "retained12", "suite-cleanup", "imports")
DEPENDENCY_HASHES = {
    "tests/memory/run_array_queries.py": "3fdd6737b988fb22a7fc170fda804460701aa800c65360d08d38fa980532603f",
    "tests/memory/run_access_contracts.py": "027993590cf6c93d7501c9def4532a1bb43f77edb6fa2e6e150b831c5778ad70",
    "tests/scalars/generate.py": "34184ce6b094760311327ee42ba69f369890caa2692ba35593f577282943f0e5",
    "tests/scalars/reference.py": "2887d51358d9de73c154091890d1eb8d4d15cb3302345b3ff9b8300c5c87d150",
    "tests/scalars/run.py": "c026bc882827a0a78ed03e6af162479b864a573746b6debd392f6834816d3840",
}
FIXTURE_HASHES = {
    "coverage.sts": "a745b6a17e84776645b1718389290303559661bd70d450f20656d82eeb2cb5f0",
    "control_anchors.sts": "3060d1ecb3730a0fca49f2e7aa8b814961da6950eba77535a836a96b083a1557",
    "retained_helper.sts": "9ef8909f8f34543dd4c7726bd3e46e52dd2a24a7e455333351cc28fa7af18b35",
    "suite_cleanup.sts": "3af00a8c6b2f435d63e0d221f58fde190c03fd7628a96afc2cf393f00e5461c3",
    "imports/root.sts": "0762e1615f1c8d27f7310ac513454cee02728460adb732af00cf388a8e2636db",
    "imports/util/query.sts": "9214eaab7a5bc113eb390c21600f592513061139b0f457547cf1779938b60d46",
}
ADAPTER_HEADER = "@[[crash: disable]];\n"
ADAPTER_SUFFIX = "\ntest 'ordinary fixed-array oracle' { expect_eq.(array_query_case(), 0); }\n"
# Literal physical-line oracles, independently read from the original generator.
# All checks and each success return are reached; same-line failure returns do
# not create extra line IDs. No allocations or observer assertions are hidden.
ADAPTER_LINES = {
    "direct": [5, 6, 7, 8], "named_extent": [5, 6, 8], "inferred_extent": [5, 6, 8],
    "empty_extent": [5, 6, 8], "empty_inferred": [5, 6, 8],
    "const_array": [5, 6, 8], "final_array": [5, 6, 8], "matrix": [5, 6, 7, 8, 9],
    "deep_rank": [5, 6, 7, 8, 9, 10, 12], "global_array": [5, 6, 7, 8, 10, 11, 13],
}
COVERAGE_LINES = [4, 5, 6, 7, 8, 11, 12, 13, 16, 17]
CONTROL_LINES = [3, 4, 5, 6, 7, 10, 11, 12, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23]
HELPER_LINES = [5, 6, 7, 8, 11, 12, 13]
IMPORT_PAIRS = [(0, 5), (0, 7), (0, 8), (0, 10), (1, 4), (1, 5), (1, 6), (1, 7),
                (1, 10), (1, 12), (1, 13), (1, 14), (1, 16)]


def load_support():
    spec = importlib.util.spec_from_file_location("array_query_source_support", ROOT / "tests/observation/test_source_imports.py")
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


support = load_support()
sha = support.sha


def ordinary_cases():
    """Reuse only the literal RuntimeCase selection; never cases/main/run dispatch."""
    for path, digest in DEPENDENCY_HASHES.items():
        assert sha(ROOT / path) == digest, "ordinary generator dependency changed: " + path
    old_path = sys.path[:]
    try:
        sys.path.insert(0, str(ROOT / "tests/memory"))
        import run_array_queries as ordinary
        assert Path(ordinary.__file__).resolve() == ROOT / "tests/memory/run_array_queries.py"
        selected = {case.name: case for case in ordinary.runtime_cases() if case.name in ORDINARY_NAMES}
    finally:
        sys.path[:] = old_path
    assert set(selected) == set(ORDINARY_NAMES)
    assert all(type(selected[name]) is ordinary.RuntimeCase for name in ORDINARY_NAMES)
    return {name: selected[name] for name in ORDINARY_NAMES}


def adapt(source):
    old = "fn main(void): i32"
    assert len(re.findall(r"\bfn\s+main\s*\(", source)) == 1, "adapter requires one main declaration"
    assert source.count(old) == 1, "adapter requires one exact main signature"
    assert "array_query_case" not in source
    assert source.startswith(ADAPTER_HEADER) and source.endswith("\n")
    assert source.count(ADAPTER_HEADER) == 1, "adapter requires one exact first-line crash-disable header"
    # Observation rejects this ordinary root header. The reviewed adapter keeps
    # its LF so every downstream physical-line oracle is unchanged.
    body = "\n" + source[len(ADAPTER_HEADER):]
    return body.replace(old, "fn array_query_case(void): i32", 1) + ADAPTER_SUFFIX


def fixture_oracle(name):
    """Expected facts come from fixed source, never a compiler manifest/report."""
    pair = lambda lines: [(0, line) for line in lines]
    # id, file, line, operation, declaration, owner, optional static test.
    sites = []
    tests, assertions, live, byte_count = 1, 1, 0, 0
    minimum, maximum, expected = 100, 0, 0
    if name.startswith("adapter-"):
        lines = pair(ADAPTER_LINES[name.removeprefix("adapter-")]); hits = lines
        return dict(pairs=lines, hits=hits, sites=sites, assertions=assertions, tests=tests,
                    live=live, byte_count=byte_count, minimum=minimum, maximum=maximum, expected=expected, sentinel=False)
    if name in ("coverage80", "coverage81", "coverage-only", "allocation-only"):
        lines = pair(COVERAGE_LINES); hits = lines[:-2]; assertions = 5
        sites = [(0, 0, 4, 1, 1, "index", None), (1, 0, 6, 3, 1, "index", None)]
        minimum = 81 if name == "coverage81" else 80
        if name == "coverage81": expected = 1
        if name == "coverage-only": maximum = None
        if name == "allocation-only": minimum = None; hits = []
    elif name == "control-anchors":
        lines = pair(CONTROL_LINES); hits = [p for p in lines if p[1] not in (15, 16, 20)]
        assertions, minimum = 2, 83
    elif name in ("retained13", "retained12", "suite-cleanup"):
        lines = pair(HELPER_LINES); hits = lines; assertions = 3
        sites = [(0, 0, 5, 1, 2, "index", None)]
        if name == "suite-cleanup":
            tests = 2; sites.append((1, 0, 20, 3, 5, "release after earlier test", 1))
        else:
            live, byte_count, maximum = 1, 13, (13 if name == "retained13" else 12)
            if name == "retained12": expected = 1
    elif name == "imports":
        lines = IMPORT_PAIRS; hits = lines; assertions, tests = 14, 2
        sites = [(0, 1, 4, 1, 5, "index", None), (1, 1, 6, 3, 5, "index", None)]
    else:
        raise ValueError("unreviewed positive: " + name)
    return dict(pairs=lines, hits=hits, sites=sites, assertions=assertions, tests=tests,
                live=live, byte_count=byte_count, minimum=minimum, maximum=maximum, expected=expected, sentinel=True)


def expected_cases(levels):
    return {"ordinary_native": [f"ordinary-{name}-O{opt}" for opt in levels for name in ORDINARY_NAMES],
            "observed_native": [f"{name}-O{opt}" for opt in levels for name in (*FIXED_NAMES, *("adapter-" + n for n in ORDINARY_NAMES))]}


# All new negatives stay no-launch-only. The strings are inputs, never implicit
# permission to execute. Both query operators have an independently named case.
NEGATIVE_FORMS = {
    "raw-pointer": ("stack i32 value=0; stack i32 *rw p=&value;", "p", ""),
    "pointer-call": ("", "get()", "fn get(void): i32 *rw { ret nil; }\n"),
    "scalar": ("stack i32 value=0;", "value", ""),
    "fully-indexed": ("stack i32 a[2]=.{};", "a[0]", ""),
    "address": ("stack i32 a[2]=.{};", "&a", ""),
    "dereference": ("stack i32 value=0; stack i32 *rw p=&value;", "*.(p)", ""),
    "cast": ("stack i32 a[2]=.{};", "(i32 *rw) a", ""),
    "ternary-root": ("stack i32 a[2]=.{}; stack i32 b[2]=.{};", "true ? a : b", ""),
    "pointer-elements": ("stack i32 *rw a[2];", "a", ""),
    "slice": ("stack i32 a[2]=.{}; stack []i32 view=a[0:1];", "view", ""),
    "member": ("stack Box box=.{.values=.{1,2}};", "box.values", "type Box:struct {ext i32 values[2];};\n"),
    "alias": ("stack Element a[2]=.{};", "a", "type Element:i32;\n"),
    "flagged": ("stack i32 a[2][3]=.{};", "a[unchecked: 0]", ""),
    "nested-native": ("stack i32 a[2][3]=.{};", "a[foreign()]", ""),
    "nested-uncalled": ("stack i32 a[2][3]=.{};", "a[false ? foreign() : 0]", ""),
    "nested-short-circuit": ("stack i32 a[2][3]=.{};", "a[(false && foreign()) ? 1 : 0]", ""),
    "nested-after-return": ("stack i32 a[2][3]=.{}; ret 0;", "a[foreign()]", ""),
    "scalar-shadow": ("stack i32 a=0;", "a", "stack i32 a[2]=.{};\n"),
    "pointer-shadow": ("stack i32 *rw a=nil;", "a", "stack i32 a[2]=.{};\n"),
    "noninteger-index": ("stack i32 a[2][3]=.{};", "a[0.5]", ""),
    "huge-selected": ("stack u8 a[2147483648];", "a", ""),
    "huge-selected-inner": ("stack u8 a[1][2147483648];", "a[0]", ""),
}


def negative_sources():
    result = {}
    tail = "test 'nonlaunch only' { print.('ARRAY_QUERY_MUST_NOT_RUN\\n'); }\n"
    for op in ("len", "cap"):
        for name, (setup, operand, declarations) in NEGATIVE_FORMS.items():
            result[f"{name}-{op}"] = {"root.sts": "mod rejected;\n" + declarations +
                f"fn unused(void): i32 {{ {setup} ret {op}.({operand}); }}\n" + tail}
        result[f"unsafe-{op}"] = {"root.sts": f"mod rejected;\nfn unused(void): i32 {{ stack i32 a[2]=.{{}}; unsafe {{ ret {op}.(a); }} }}\n" + tail}
        result[f"parameter-{op}"] = {"root.sts": f"mod rejected;\nfn query(stack i32 a[2]): i32 {{ ret {op}.(a); }}\n" + tail}
        result[f"global-initializer-{op}"] = {"root.sts": f"mod rejected;\nstack i32 a[2]=.{{}};\nstack i32 n={op}.(a);\n" + tail}
        result[f"comptime-{op}"] = {"root.sts": f"mod rejected;\nfn unused(void): void {{ stack i32 a[2]=.{{}}; @comptime assert.({op}.(a) == 2); }}\n" + tail}
        result[f"import-shadow-{op}"] = {"root.sts": "mod rejected;\nimp dep;\nstack i32 a[2]=.{};\n" + tail,
            "dep.sts": f"mod dep;\nstack i32 a=0;\nfn unused(void): i32 {{ ret {op}.(a); }}\n"}
        result[f"sibling-shadow-{op}"] = {"root.sts": "mod rejected;\nimp left;\nimp right;\n" + tail,
            "left.sts": "mod left;\nstack i32 a[2]=.{};\n",
            "right.sts": f"mod right;\nstack i32 a=0;\nfn unused(void): i32 {{ ret {op}.(a); }}\n"}
    return result


def boundary_sources():
    # Primitive-array boundary reasoning only: these huge objects can never be
    # built/linked/executed by native dispatch, even if their source is accepted.
    result = {}
    for op in ("len", "cap"):
        result["exact-int32-" + op] = "mod boundary;\nfn query(void): i32 { stack u8 a[2147483647]; ret " + op + ".(a); }\ntest 'nonlaunch boundary' { }\n"
        result["nonselected-huge-" + op] = "mod boundary;\nfn query(void): i32 { stack u8 a[2147483648][3]; ret " + op + ".(a[0]); }\ntest 'nonlaunch boundary' { }\n"
    return result


def nonlaunch_inventory(levels):
    return {"compile_only": ["reject-" + name for name in negative_sources()] +
        [f"boundary-ir-{name}-O{opt}" for opt in levels for name in boundary_sources()] +
        [f"ordinary-ir-{name}-O{opt}" for opt in levels for name in ORDINARY_NAMES] +
        [f"observed-ir-{name}-O{opt}" for opt in levels for name in (*FIXED_NAMES, *("adapter-" + n for n in ORDINARY_NAMES))]}


def materialize(session):
    for name, digest in FIXTURE_HASHES.items():
        assert sha(FIXTURES / name) == digest, "literal fixture changed: " + name
    sources = session.artifacts / "sources"
    ordinary, adapted, transformations = {}, {}, {}
    for name, case in ordinary_cases().items():
        original = sources / "ordinary" / (name + ".sts")
        adapter = sources / "adapted" / (name + ".sts")
        original.parent.mkdir(parents=True, exist_ok=True); adapter.parent.mkdir(parents=True, exist_ok=True)
        original.write_bytes(case.source.encode()); adapter.write_bytes(adapt(case.source).encode())
        ordinary[name], adapted["adapter-" + name] = original, adapter
        transformations[name] = {"original": str(original.relative_to(session.artifacts)), "original_sha256": sha(original),
            "adapted": str(adapter.relative_to(session.artifacts)), "adapted_sha256": sha(adapter),
            "exact_replacement": ["fn main(void): i32", "fn array_query_case(void): i32"],
            "replacement_count": 1, "appended_utf8": ADAPTER_SUFFIX,
            "header_replacement": [ADAPTER_HEADER, "\n"], "header_replacement_count": 1,
            "ordinary_crash_disable_header_preserved": True, "observed_crash_disable_header_removed": True,
            "downstream_physical_lines_preserved": True,
            "eligible_lines": ADAPTER_LINES[name], "hit_lines": ADAPTER_LINES[name],
            "assertions": 1, "tests": 1, "sites": [], "live_allocations": 0, "outstanding_bytes": 0}
    files = {name: (FIXTURES / name).read_text() for name in FIXTURE_HASHES}
    selected = {"coverage80": "coverage.sts", "coverage81": "coverage.sts", "coverage-only": "coverage.sts", "allocation-only": "coverage.sts",
                "control-anchors": "control_anchors.sts", "retained13": "retained_helper.sts", "retained12": "retained_helper.sts", "suite-cleanup": "suite_cleanup.sts"}
    positive = {name: support.write_sources(sources / "fixed" / name, {"root.sts": files[fixture]}) for name, fixture in selected.items()}
    positive["imports"] = support.write_sources(sources / "fixed/imports", {name.removeprefix("imports/"): value for name, value in files.items() if name.startswith("imports/")})
    positive.update(adapted)
    negative = {name: support.write_sources(sources / "nonlaunch" / name, files) for name, files in negative_sources().items()}
    for name, source in boundary_sources().items():
        support.write_sources(sources / "boundaries" / name, {"root.sts": source})
    session.freeze_generated()
    (session.artifacts / "adapter-transformations.json").write_text(json.dumps(transformations, indent=2) + "\n")
    (session.artifacts / "literal-oracles.json").write_text(json.dumps({name: fixture_oracle(name) for name in positive}, indent=2) + "\n")
    session.transformation_hash = sha(session.artifacts / "adapter-transformations.json")
    return ordinary, positive, negative


def config_digest(compiler, runtime, optimization, *, enabled=3, warn=1, maximum=8, minimum=90, policy=POLICY, debug=0, target="x86_64-unknown-linux-gnu"):
    """Independent v3 serializer shared by all observed-family validators."""
    return support.config_digest(compiler, runtime, optimization, enabled=enabled, warn=warn,
        maximum=maximum, minimum=minimum, policy=policy, debug=debug, target=target)


def run_ordinary(session, sources, levels):
    for opt in levels:
        for name in ORDINARY_NAMES:
            key = f"ordinary-{name}-O{opt}"; binary = session.artifacts / (key + ".program")
            pre, post = session.artifacts / (key + ".pre.ll"), session.artifacts / (key + ".post.ll")
            env = dict(os.environ, STS_DUMP_IR_PRE=str(pre), STS_DUMP_IR=str(post))
            _, compiled = session.command(key + "-build", [session.compiler, "build", sources[name], f"-o={opt}", "-o", binary], env=env)
            assert not support.POSITIVE_NONLAUNCH_FAILURES.search(compiled.stdout + compiled.stderr)
            assert binary.is_file() and not binary.is_symlink() and os.access(binary, os.X_OK)
            for path in (pre, post): assert path.is_file() and "__stasha_obs_" not in path.read_text()
            _, executed = session.command(key + "-execute", [binary])
            assert executed.stdout == executed.stderr == b""
            session.cases.append({"name": key, "stage": "ordinary_native", "status": "pass", "executions": 1,
                "source_sha256": sha(sources[name]), "binary_sha256": sha(binary), "pre_ir_sha256": sha(pre), "post_ir_sha256": sha(post)})


def flags_for(oracle):
    flags = []
    if oracle["maximum"] is not None: flags += ["--max-leaked-bytes", str(oracle["maximum"])]
    if oracle["minimum"] is not None: flags += ["--min-test-coverage", str(oracle["minimum"])]
    return flags


def run_observed(session, sources, levels):
    runs, evidence_paths, signatures = set(), set(), {}
    for opt in levels:
        for name in (*FIXED_NAMES, *("adapter-" + n for n in ORDINARY_NAMES)):
            oracle = fixture_oracle(name); key = f"{name}-O{opt}"; entry = sources[name]
            binary = session.artifacts / (key + ".test")
            pre, post = session.artifacts / (key + ".pre.ll"), session.artifacts / (key + ".post.ll")
            env = dict(os.environ, STS_DUMP_IR_PRE=str(pre), STS_DUMP_IR=str(post))
            dest, result = session.command(key, [session.compiler, "test", entry, f"-o={opt}", "-o", binary, *flags_for(oracle)], expected=oracle["expected"], env=env)
            assert result.stdout.count(b"OBSERVATION_ONCE\n") == int(oracle["sentinel"])
            match = re.search(rb"observation: private evidence ([^\r\n]+)", result.stderr); assert match
            evidence = Path(os.fsdecode(match.group(1)))
            assert str(evidence.resolve()) not in evidence_paths and not evidence.is_symlink()
            evidence_paths.add(str(evidence.resolve()))
            metadata, report = (evidence / "driver.json").read_bytes(), (evidence / "report").read_bytes()
            assert len(metadata) <= 33554432 and len(report) <= 2097152
            (dest / "driver.json").write_bytes(metadata); (dest / "report").write_bytes(report)
            data = support.strict_json(metadata)
            assert data["run"] not in runs, "stale runtime identity"; runs.add(data["run"])
            raw = support.validate_driver(data, report.decode("ascii"), session, binary, entry,
                **{key: oracle[key] for key in ("expected", "pairs", "hits", "assertions", "tests", "live", "byte_count")})
            enabled = (1 if oracle["maximum"] is not None else 0) | (2 if oracle["minimum"] is not None else 0)
            assert data["enabled"] == enabled and data["warn_memleak"] is False
            assert data["max_leaked_bytes"] == oracle["maximum"] and data["min_test_coverage"] == oracle["minimum"]
            assert re.fullmatch(r"x86_64-(?:unknown|pc)-linux-gnu", data["target"]), "unqualified target"
            assert data["config_sha256"] == config_digest(session.compiler_hash, session.runtime_hash, opt,
                enabled=enabled, warn=0, maximum=oracle["maximum"], minimum=oracle["minimum"], target=data["target"])[0]
            assert [(s["id"], s["file_id"], s["line"], s["kind"], s["decl_id"], s["owner"], s["static_test_id"]) for s in data["sites"]] == oracle["sites"]
            if name in ("coverage80", "coverage81", "coverage-only"): assert raw.hex() == "ff00"
            if name == "allocation-only": assert raw.hex() == "0000" and len(data["coverage"]) == 10
            if name == "control-anchors": assert raw.hex() == "ffb903"
            if name == "imports": assert raw.hex() == "ff1f"
            if name.startswith("retained"):
                assert data["allocations"] == [{"generation": 1, "domain": 1, "bytes": 13, "allocation_site": 0, "resize_site": 4294967295, "test": 0}]
            for path in (pre, post):
                text = path.read_text(); assert "__stasha_obs_init_v3" in text
                assert not re.search(r"@__stasha_obs_init\(", text)
            signature = (data["coverage"], [(f["lines"], f["exclusions"]) for f in data["files"]], data["tests"], data["sites"])
            if name in signatures: assert signature == signatures[name], "optimization changed fixed source inventory"
            signatures[name] = signature
            session.cases.append({"name": key, "stage": "observed_native", "status": "pass", "executions": 1,
                "input_graph_sha256": data["input_graph_sha256"], "map_sha256": data["map_sha256"], "config_sha256": data["config_sha256"],
                "source_sha256": sha(entry), "binary_sha256": sha(binary), "run_id": data["run"], "pre_ir_sha256": sha(pre), "post_ir_sha256": sha(post),
                "assertions_passed": oracle["assertions"], "tests": oracle["tests"], "eligible": len(oracle["pairs"]),
                "covered": len(oracle["hits"]), "live_allocations": oracle["live"], "tracked_outstanding_bytes": oracle["byte_count"]})


def rejection_diagnostic(name, result):
    stderr = result.stderr
    assert result.returncode == 1 and b"ARRAY_QUERY_MUST_NOT_RUN" not in result.stdout
    # The exact driver footer follows expected source diagnostics; internal,
    # verifier and optimizer failures must still fail these negative controls.
    footer = re.compile(rb"\(EE\) \[[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\] code generation failed\n")
    diagnostic_text = result.stdout + b"\n" + b"".join(
        line for line in stderr.splitlines(keepends=True) if not footer.fullmatch(line))
    assert not support.POSITIVE_NONLAUNCH_FAILURES.search(diagnostic_text)
    assert re.search(rb"(?:root|dep|right)\.sts:[1-9][0-9]*", stderr), "missing owned source span"
    if name.startswith("noninteger-index-"): required = b"array index must have integer type"
    elif name.startswith("huge-selected"):
        required = ("fixed-array " + name.rsplit("-", 1)[1] + " extent exceeds INT32_MAX").encode()
    else: required = b"unsupported by observation profile"
    assert required in stderr, (name, "wrong rejection", stderr)
    assert not re.search(rb"linking|archiving|link_object|archive_object", stderr, re.I)


def run_nonlaunch(session, ordinary, positive, negative, levels, ir_compiler):
    ir_compiler = ir_compiler.resolve(strict=True); digest = sha(ir_compiler)
    assert ir_compiler != session.compiler and digest != session.compiler_hash
    session.ir_compiler_hash = digest
    for name, source in negative.items():
        key = "reject-" + name; output = session.artifacts / (key + ".must-not-exist")
        pre, post = session.artifacts / (key + ".pre.ll"), session.artifacts / (key + ".post.ll")
        dest, result = session.command(key, [ir_compiler, "test", source, "--max-leaked-bytes", "0", "-o", output],
            expected=1, env=dict(os.environ, STS_DUMP_IR_PRE=str(pre), STS_DUMP_IR=str(post)))
        rejection_diagnostic(name, result); support.retain_nonlaunch_evidence(dest, result)
        assert not output.exists() and not pre.exists() and not post.exists()
        session.cases.append({"name": key, "stage": "compile_only", "status": "pass", "executions": 0})
    for opt in levels:
        controls = [("ordinary-ir-" + name, "build", source, []) for name, source in ordinary.items()]
        controls += [("boundary-ir-" + name, "test", session.artifacts / "sources/boundaries" / name / "root.sts", ["--max-leaked-bytes", "0"]) for name in boundary_sources()]
        controls += [("observed-ir-" + name, "test", source, flags_for(fixture_oracle(name))) for name, source in positive.items()]
        for prefix, command, source, flags in controls:
            key = f"{prefix}-O{opt}"; output = session.artifacts / (key + ".must-not-exist")
            pre, post = session.artifacts / (key + ".pre.ll"), session.artifacts / (key + ".post.ll")
            dest, result = session.command(key, [ir_compiler, command, source, f"-o={opt}", "-o", output, *flags],
                expected=1, env=dict(os.environ, STS_DUMP_IR_PRE=str(pre), STS_DUMP_IR=str(post)))
            support.validate_positive_nonlaunch_result(result, output); support.retain_nonlaunch_evidence(dest, result)
            for path in (pre, post):
                text = path.read_text()
                assert ("__stasha_obs_init_v3" in text) == (command == "test")
            session.cases.append({"name": key, "stage": "compile_only", "status": "pass", "executions": 0,
                "pre_ir_sha256": sha(pre), "post_ir_sha256": sha(post), "independent_ir_review": "required"})
    assert sha(ir_compiler) == digest


def selftest(artifacts):
    class PureTests(unittest.TestCase):
        def test_exact_ordinary_selection(self):
            selected = ordinary_cases(); self.assertEqual(tuple(selected), ORDINARY_NAMES)
            for name, case in selected.items():
                converted = adapt(case.source)
                restored = converted[:-len(ADAPTER_SUFFIX)].replace("fn array_query_case(void): i32", "fn main(void): i32", 1)
                self.assertTrue(restored.startswith("\n"))
                self.assertEqual(ADAPTER_HEADER + restored[1:], case.source)
                self.assertEqual(restored.count("\n"), case.source.count("\n"))
                self.assertNotIn(ADAPTER_HEADER, converted)
                self.assertEqual(converted.count("fn array_query_case(void): i32"), 1)
                self.assertNotIn("fn main(", converted)
                self.assertEqual(converted.count("expect_eq.(array_query_case(), 0)"), 1)
        def test_adapter_rejects_ambiguous_inputs(self):
            valid = ordinary_cases()["direct"].source
            for bad in (valid + valid, valid.replace("fn main", "fn other"), valid.replace("main(void)", "main( void )"), valid + "// array_query_case\n", valid.split("\n", 1)[1]):
                with self.subTest(source=bad): self.assertRaises(AssertionError, adapt, bad)
        def test_adapter_requires_exact_first_header(self):
            valid = ordinary_cases()["direct"].source
            invalid = (valid[len(ADAPTER_HEADER):], ADAPTER_HEADER + valid, "\n" + valid,
                       valid.replace(ADAPTER_HEADER, "@[[crash:disable]];\n", 1),
                       valid.replace(ADAPTER_HEADER, "@[[crash: disable]];\r\n", 1),
                       valid + ADAPTER_HEADER)
            for bad in invalid:
                with self.subTest(source=bad): self.assertRaises(AssertionError, adapt, bad)
        def test_literal_fixture_hashes(self):
            self.assertEqual({str(p.relative_to(FIXTURES)) for p in FIXTURES.rglob("*.sts")}, set(FIXTURE_HASHES))
            for name, digest in FIXTURE_HASHES.items(): self.assertEqual(sha(FIXTURES / name), digest)
        def test_literal_oracles(self):
            central = fixture_oracle("coverage80")
            self.assertEqual(central["pairs"], [(0, n) for n in [4,5,6,7,8,11,12,13,16,17]])
            self.assertEqual(central["hits"], central["pairs"][:-2]); self.assertEqual(central["assertions"], 5)
            self.assertTrue(8 * 100 >= 80 * 10); self.assertFalse(8 * 100 >= 81 * 10)
            self.assertEqual(fixture_oracle("allocation-only")["pairs"], central["pairs"])
            self.assertEqual(fixture_oracle("allocation-only")["hits"], [])
            self.assertEqual(len(fixture_oracle("control-anchors")["hits"]), 15)
            self.assertEqual(fixture_oracle("imports")["assertions"], 14)
        def test_independent_configuration_known_answers(self):
            digest, encoded = config_digest("dd" * 32, "aa" * 32, 2)
            self.assertEqual(len(encoded), 526)
            self.assertEqual(digest, "1dc9f3d3e2be65e986cf997e1350dea939ad3517d392f67a3d9220f98d3ed457")
            old, old_bytes = config_digest("dd" * 32, "aa" * 32, 2, policy="closed-direct-source-fixed-array-queries-v3")
            self.assertNotEqual(old, digest); self.assertNotEqual(old_bytes, encoded)
        def test_nonlaunch_partition(self):
            inventory = expected_cases([0, 2, 3])
            self.assertEqual(len(inventory["ordinary_native"]), 30)
            self.assertEqual(len(inventory["observed_native"]), 57)
            self.assertFalse(set(sum(inventory.values(), [])) & set(nonlaunch_inventory([0,2,3])["compile_only"]))
            self.assertTrue(all(not name.startswith(("reject-", "boundary-")) for names in inventory.values() for name in names))
            self.assertEqual(set(boundary_sources()), {"exact-int32-len", "exact-int32-cap", "nonselected-huge-len", "nonselected-huge-cap"})
        def test_preparation_is_nonterminal(self):
            with tempfile.TemporaryDirectory() as temp:
                session = support.FamilySession(FAMILY, None, Path(temp), [0,2,3], runtime=True)
                session.nonlaunch_required = nonlaunch_inventory(session.levels)["compile_only"]
                original, positive, negative = materialize(session)
                self.assertEqual(tuple(original), ORDINARY_NAMES)
                self.assertEqual(set(positive), set(FIXED_NAMES) | {"adapter-" + n for n in ORDINARY_NAMES})
                self.assertEqual(set(negative), set(negative_sources()))
                session.write_inventory(expected_cases(session.levels)); session.status = "prepared"; session.receipt()
                data = json.loads((session.artifacts / "result.json").read_text())
                self.assertEqual(data["status"], "prepared"); self.assertFalse(data["runs"])
                self.assertEqual(data["separate_no_launch_controls"]["status"], "NOT_RUN")
                session.verify_inputs()
                next(iter(original.values())).write_text("changed")
                self.assertRaises(AssertionError, session.verify_inputs)
        def test_owned_negative_diagnostic_and_footer(self):
            valid = subprocess.CompletedProcess([], 1, b"", b"root.sts:3:1: array index must have integer type\n(EE) [2026-10-12 02:00:00] code generation failed\n")
            rejection_diagnostic("noninteger-index-len", valid)
            footer = b"(EE) [2026-10-12 02:00:00] code generation failed\n"
            malformed = (b"code generation failed\n", b"prefix " + footer,
                         footer.rstrip(b"\n") + b" suffix\n", footer.replace(b"02:00:00", b"not-a-time"),
                         footer.replace(b"(EE)", b"(XX)"), footer.rstrip(b"\n"))
            for bad in (b"root.sts:3:1: other error", valid.stderr + b"LLVM IR verification failed", valid.stderr + b"linking failed",
                        *(valid.stderr + text for text in malformed)):
                self.assertRaises(AssertionError, rejection_diagnostic, "noninteger-index-len", subprocess.CompletedProcess([], 1, b"", bad))
            self.assertRaises(AssertionError, rejection_diagnostic, "noninteger-index-len", subprocess.CompletedProcess([], 1, footer, valid.stderr))
        def test_unknown_family_rejected(self):
            with tempfile.TemporaryDirectory() as temp:
                self.assertRaises(AssertionError, support.FamilySession, "invented-family", None, Path(temp), [0], runtime=False)
    with mock.patch("subprocess.run", side_effect=AssertionError("pure tests must not spawn any process")):
        result = unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromTestCase(PureTests))
    artifacts.mkdir(parents=True, exist_ok=True)
    (artifacts / "result.json").write_text(json.dumps({"schema": 2, "family": "array-query-selftest", "status": "pass" if result.wasSuccessful() else "fail",
        "tests_run": result.testsRun, "failures": len(result.failures), "errors": len(result.errors), "native_executions": 0}, indent=2) + "\n")
    return 0 if result.wasSuccessful() else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path); parser.add_argument("--ir-compiler", type=Path)
    parser.add_argument("--artifacts", required=True, type=Path); parser.add_argument("--opt", default="0,1,2,3")
    parser.add_argument("--cc", default="cc", help="wrapper-selected host compiler identity; runner does not build compilers")
    parser.add_argument("--stage", choices=("all", "prepare", "compile-only", "native"), default="all")
    parser.add_argument("--selftest-only", action="store_true")
    args = parser.parse_args()
    if args.selftest_only: return selftest(args.artifacts)
    levels = support.parse_levels(args.opt, parser)
    if args.stage != "prepare" and args.compiler is None: parser.error("--compiler required")
    if args.stage == "compile-only" and args.ir_compiler is None: parser.error("--ir-compiler required for no-launch-only controls")
    session = support.FamilySession(FAMILY, args.compiler, args.artifacts, levels, runtime=True)
    session.nonlaunch_required = nonlaunch_inventory(levels)["compile_only"]
    try:
        ordinary, positive, negative = materialize(session)
        inventory = nonlaunch_inventory(levels) if args.stage == "compile-only" else expected_cases(levels)
        session.write_inventory(inventory)
        if args.stage == "prepare": session.status = "prepared"
        elif args.stage == "compile-only":
            run_nonlaunch(session, ordinary, positive, negative, levels, args.ir_compiler); session.finish(inventory)
        else:
            run_ordinary(session, ordinary, levels); run_observed(session, positive, levels); session.finish(inventory)
        assert sha(session.artifacts / "adapter-transformations.json") == session.transformation_hash
    except BaseException as error:
        session.status, session.failure = "fail", repr(error); raise
    finally:
        session.receipt()
    print(f"{session.status.upper()}: bounded array-query {args.stage}; evidence {session.artifacts}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
