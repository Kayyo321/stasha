#!/usr/bin/env python3
"""Direct-source/positional-slice v4 qualification with distinct preparation, rejection and native stages.

--selftest-only is pure Python/mocks: it never invokes Stasha, a C compiler, or a
generated program. Compile-only controls require a separate no-launch compiler.
Native execution is only appropriate after independent source/pre/post-IR review.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import struct
import sys
import tempfile
import unittest
from unittest import mock

if not __debug__:
    raise RuntimeError("qualification requires Python assertions enabled")
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
BASIC = ROOT / "tests/observation/fixtures/source_imports/basic"
PROFILE = "native-sequential-direct-source-positional-slice-v4"
GRAPH_PROFILE = "native-sequential-direct-source-v2"
BASIC_HASHES = {"root.sts": "d96b7a0bcd97e1e90a2420e768d5348fa4b063cde8e35be3a5603f0bbc2db61d",
                "util/calc.sts": "b7dca2e313dc1f404c3923027aa17f5656f17acc9645324092ebd9009e87566b"}
BASIC_LINES = [(0, 4), (1, 3), (1, 5), (1, 6), (1, 7), (1, 8), (1, 9), (1, 12)]
BASIC_HITS = BASIC_LINES[:-1]
FAMILY_IDENTITIES = {
    "ordinary-imported-tests": ("tests/scalars/test_imported_tests.py", False),
    "observed-source-imports": ("tests/observation/test_source_imports.py", True),
    "observed-array-queries": ("tests/observation/test_array_queries.py", True),
    "observed-primitive-slices": ("tests/observation/test_primitive_slices.py", True),
    "observed-positional-slices": ("tests/observation/test_positional_slices.py", True),
}
MANDATORY_INPUTS = ['tests/memory/run_positional_make.py', 'tests/memory/test_positional_make.py', 'tests/observation/test_positional_slices.py', 'tests/observation/fixtures/positional_slices/corpus.json', 'tests/observation/fixtures/positional_slices/coverage.sts', 'tests/observation/fixtures/positional_slices/retained.sts', 'tests/observation/fixtures/positional_slices/imports/root.sts', 'tests/observation/fixtures/positional_slices/imports/util/leaf.sts'] + ["tests/observation/test_primitive_slices.py", "tests/observation/test_array_queries.py",
    "tests/memory/run_array_queries.py", "tests/memory/run_access_contracts.py",
    "tests/scalars/generate.py", "tests/scalars/reference.py", "tests/scalars/run.py", "tests/scalars/test_imported_tests.py", "tests/observation/test_source_imports.py",
    "tests/observation/test_observation.py", "tests/observation/test_source_graph.c",
    "tests/observation/test_driver.c", "tests/observation/runtime_ledger_tests.c",
    "src/common/observation_sources.h", "src/common/observation_options.h",
    "src/common/observation_driver.h", "src/codegen/cg_observation.c",
    "src/runtime/observation_runtime.c", "src/runtime/observation_runtime.h", "Makefile",
    *["tests/observation/fixtures/source_imports/basic/" + name for name in BASIC_HASHES]]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def parse_levels(text, parser=None):
    try:
        levels = [int(item) for item in text.split(",")]
        assert levels and len(levels) == len(set(levels)) and all(item in range(4) for item in levels)
    except (ValueError, AssertionError):
        if parser: parser.error("--opt must be distinct comma-separated levels 0 through 3")
        raise ValueError("invalid optimization set")
    return levels


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def input_manifest():
    paths = set(ROOT.glob("src/**/*.c")) | set(ROOT.glob("src/**/*.h"))
    paths |= set((ROOT / "tests/observation").glob("*.c"))
    paths |= set((ROOT / "tests/observation/fixtures").rglob("*.sts"))
    paths |= {ROOT / value for value in MANDATORY_INPUTS}
    paths |= {ROOT / "std/crypto/sha256_rt.c", ROOT / "std/crypto/sha256_rt.h"}
    return {str(path.relative_to(ROOT)): sha(path) for path in sorted(paths)}


class FamilySession:
    """Fail-closed recorder with explicit runner/runtime identity for all five families."""
    def __init__(self, family, compiler, artifacts, levels, *, runtime):
        assert family in FAMILY_IDENTITIES, "unknown qualification family"
        assert runtime == FAMILY_IDENTITIES[family][1], "wrong family runtime requirement"
        self.family, self.levels = family, levels
        artifacts = artifacts.resolve(); artifacts.mkdir(parents=True, exist_ok=True)
        self.artifacts = Path(tempfile.mkdtemp(prefix="run-", dir=artifacts))
        (artifacts / "LATEST").write_text(str(self.artifacts) + "\n")
        self.compiler = compiler.resolve() if compiler else None
        self.compiler_hash = sha(self.compiler) if self.compiler else None
        self.runtime = self.compiler.parent / "observation_runtime.a" if runtime and self.compiler else None
        self.runtime_hash = sha(self.runtime) if self.runtime else None
        self.source_manifest = input_manifest()
        self.generated = {}
        self.generated_resolutions = {}
        self.records, self.cases = [], []
        self.status, self.failure = "running", None
        self.inventory = {}
        (self.artifacts / "source-manifest.json").write_text(json.dumps(self.source_manifest, indent=2) + "\n")

    def freeze_generated(self):
        self.generated = {str(path.relative_to(self.artifacts)): sha(path)
                          for path in sorted((self.artifacts / "sources").rglob("*.sts"))}
        self.generated_resolutions = {name: str((self.artifacts / name).resolve()) for name in self.generated}
        (self.artifacts / "generated-manifest.json").write_text(json.dumps(self.generated, indent=2) + "\n")

    def verify_inputs(self):
        if self.compiler: assert sha(self.compiler) == self.compiler_hash, "compiler changed"
        if self.runtime: assert sha(self.runtime) == self.runtime_hash, "runtime changed"
        assert input_manifest() == self.source_manifest, "source inventory changed or disappeared"
        assert json.loads((self.artifacts / "source-manifest.json").read_text()) == self.source_manifest, "recorded source inventory changed"
        for name, digest in self.generated.items():
            assert sha(self.artifacts / name) == digest, "generated source changed: " + name
            assert str((self.artifacts / name).resolve()) == self.generated_resolutions[name], "generated source rebound: " + name
        if self.generated:
            assert {str(p.relative_to(self.artifacts)) for p in (self.artifacts / "sources").rglob("*.sts")} == set(self.generated)

    def command(self, name, argv, *, expected=0, env=None, cwd=None):
        dest = self.artifacts / name; dest.mkdir(exist_ok=False)
        argv = list(map(str, argv))
        (dest / "command.json").write_text(json.dumps(argv, indent=2) + "\n")
        record = {"name": name, "command": argv, "status": "running"}
        self.records.append(record)
        self.verify_inputs()
        try:
            result = subprocess.run(argv, cwd=cwd or ROOT, env=env, capture_output=True, timeout=120)
        except BaseException as error:
            record.update(status="fail", error=repr(error))
            if isinstance(error, subprocess.TimeoutExpired):
                (dest / "stdout").write_bytes(error.stdout or b"")
                (dest / "stderr").write_bytes(error.stderr or b"")
            raise
        (dest / "stdout").write_bytes(result.stdout); (dest / "stderr").write_bytes(result.stderr)
        record.update(returncode=result.returncode, expected=expected,
                      status="pass" if result.returncode == expected else "fail")
        (dest / "status.json").write_text(json.dumps(record, indent=2) + "\n")
        self.verify_inputs()
        assert result.returncode == expected, (name, result.returncode, result.stderr.decode(errors="replace"))
        return dest, result

    def write_inventory(self, inventory):
        self.inventory = inventory
        (self.artifacts / "case-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")

    def finish(self, expected):
        self.verify_inputs()
        names = [case["name"] for case in self.cases]
        assert len(names) == len(set(names))
        assert sorted(names) == sorted(name for cases in expected.values() for name in cases), "missing required case"
        assert all(case["status"] == "pass" for case in self.cases), "non-pass case"
        for stage, names in expected.items():
            assert sorted(case["name"] for case in self.cases if case["stage"] == stage) == sorted(names)
        assert all(record["status"] == "pass" for record in self.records)
        observed_runs = [case['run_id'] for case in self.cases if case['stage'] == 'observed_native']
        assert len(observed_runs) == len(set(observed_runs)), 'duplicate observed run'
        self.artifact_manifest = {str(p.relative_to(self.artifacts)): sha(p) for p in sorted(self.artifacts.rglob('*'))
                                  if p.is_file() and p.name != 'result.json'}
        self.status = "pass"

    def receipt(self):
        nonlaunch = getattr(self, "nonlaunch_required", None)
        if nonlaunch is None: nonlaunch = compile_only_inventory(self.levels)["compile_only"]
        data = {"schema": 2, "family": self.family, "status": self.status, "failure": self.failure,
                "compiler_sha256": self.compiler_hash, "runtime_sha256": self.runtime_hash,
                "observation_profile": PROFILE if self.runtime else None,
                "native_oom": "NOT_RUN / outside initial native qualification",
                "runner_sha256": self.source_manifest[FAMILY_IDENTITIES[self.family][0]],
                "optimization_levels": self.levels, "source_manifest": self.source_manifest,
                "source_manifest_sha256": sha(self.artifacts / "source-manifest.json"),
                "generated_manifest": self.generated, "generated_resolutions": self.generated_resolutions,
                "ir_compiler_sha256": getattr(self, "ir_compiler_hash", None),
                "separate_no_launch_controls": {"status": "NOT_RUN" if "compile_only" not in self.inventory else self.status,
                    "required": nonlaunch,
                    "required_count": len(nonlaunch),
                    "executed_count": sum(case["stage"] == "compile_only" for case in self.cases)},
                "qualification_contract": getattr(self, "qualification_contract", None),
                "inventory": self.inventory,
                "counts": {stage: sum(case["stage"] == stage for case in self.cases) for stage in self.inventory},
                "cases": self.cases, "runs": self.records,
                "artifact_manifest": getattr(self, "artifact_manifest", {})}
        (self.artifacts / "result.json").write_text(json.dumps(data, indent=2) + "\n")


# Rejection fixture functions are uncalled; tests emit a marker or are empty.
# BENIGN_REJECTIONS may use the production CLI and can execute if wrongly admitted.
# NO_LAUNCH_NAMES remain confined to the separately reviewed no-launch compiler.
_TEST = "test 'compile only' { print.('REJECT_IMAGE_MUST_NOT_RUN\\n'); }\n"
_ROOT = "mod root;\nimp dep;\n" + _TEST
_LEAF = "mod dep;\next fn value(void): i32 { ret 7; }\n"
REJECTIONS = {
    "missing-import": {"root.sts": _ROOT},
    "leaf-import": {"root.sts": _ROOT, "dep.sts": "mod dep;\nimp absent;\n"},
    "module-mismatch": {"root.sts": _ROOT, "dep.sts": "mod other;\n"},
    "duplicate-module": {"root.sts": "mod root;\nimp dep;\nimp dep;\n" + _TEST, "dep.sts": "mod dep;\n"},
    "duplicate-canonical": {"root.sts": "mod root;\nimp dep;\nimp other;\n" + _TEST, "dep.sts": "mod dep;\n", "@symlink:other.sts": "dep.sts"},
    "self-import": {"root.sts": "mod root;\nimp self;\n" + _TEST, "@symlink:self.sts": "root.sts"},
    "alias-collision": {"root.sts": "mod root;\nimp a.same;\nimp b.same;\n" + _TEST, "a/same.sts": "mod a.same;\n", "b/same.sts": "mod b.same;\n"},
    "explicit-alias": {"root.sts": "mod root;\nimp dep = alias;\n" + _TEST, "dep.sts": _LEAF},
    "private-qualified-call": {"root.sts": "mod root;\nimp dep;\nfn unused(void): i32 { ret dep.value(); }\n" + _TEST, "dep.sts": _LEAF.replace("ext fn", "fn")},
    "private-root-fallback": {"root.sts": "mod root;\nimp dep;\nfn private_root(void): i32 { ret 7; }\n" + _TEST, "dep.sts": "mod dep;\nfn unused(void): i32 { ret private_root(); }\n"},
    "receiver-local-collision": {"root.sts": "mod root;\nimp dep;\nfn unused(void): i32 { stack i32 dep = 1; ret dep.value(); }\n" + _TEST, "dep.sts": _LEAF},
    "receiver-root-collision": {"root.sts": "mod root;\nimp dep;\nstack i32 dep = 1;\nfn unused(void): i32 { ret dep.value(); }\n" + _TEST, "dep.sts": _LEAF},
    "duplicate-declaration": {"root.sts": _ROOT, "dep.sts": "mod dep;\nfn same(void): i32 { ret 1; }\nstack i32 same = 2;\n"},
    "emitted-name-collision": {"root.sts": "mod root;\nimp dep;\nfn dep__value(void): i32 { ret 1; }\n" + _TEST, "dep.sts": _LEAF},
    "imported-main-collision": {"root.sts": "mod root;\nimp dep;\nfn dep__main(void): i32 { ret 1; }\n" + _TEST, "dep.sts": "mod dep;\nfn main(void): i32 { ret 2; }\n"},
    "identifier-truncation": {"root.sts": _ROOT, "dep.sts": "mod dep;\nfn " + "n" * 128 + "(void): i32 { ret 1; }\n"},
    "dotted-truncation": {"root.sts": "mod root;\nimp " + ".".join(["n" * 64] * 4) + ";\n" + _TEST},
    "label-truncation": {"root.sts": _ROOT, "dep.sts": "mod dep;\ntest '" + "x" * 4097 + "' { }\n"},
    "imported-header": {"root.sts": _ROOT, "dep.sts": "@[[crash: disable]];\n" + _LEAF},
    "imported-macro": {"root.sts": _ROOT, "dep.sts": "mod dep;\nint macro let hidden! = 1;\n"},
    "inactive-import": {"root.sts": "mod root;\ncomptime_if os == 'not-a-target' { imp absent; }\n" + _TEST},
    "inactive-leaf-import": {"root.sts": _ROOT, "dep.sts": "mod dep;\ncomptime_if os == 'not-a-target' { imp absent; }\n"},
    "syntax-in-leaf": {"root.sts": _ROOT, "dep.sts": "mod dep;\nfn broken( {\n"},
    "nul-source": {"root.sts": _ROOT, "dep.sts": "mod dep;\n\0"},
    "test-limit": {"root.sts": "mod root;\n" + "test 'limit' { }\n" * 4097},
    "physical-line-limit": {"root.sts": "mod root;\n" + "\n" * 1000000 + _TEST},
    "snapshot-byte-limit": {"root.sts": "mod root;\n/*" + "x" * (16 * 1024 * 1024) + "*/\n" + _TEST},
    "source-file-limit": {"root.sts": "mod root;\n" + "".join(f"imp d{i};\n" for i in range(256)) + _TEST,
                          **{f"d{i}.sts": f"mod d{i};\n" for i in range(256)}},
}

# Independently reviewed benign rejection-expected fixtures may execute their
# defined-behavior marker/empty tests if admission is broken. They are not called
# compile-only. Namespace/definition collision controls remain no-launch-only.
NO_LAUNCH_NAMES = {"duplicate-declaration", "emitted-name-collision", "imported-main-collision"}
BENIGN_REJECTIONS = {name: source for name, source in REJECTIONS.items() if name not in NO_LAUNCH_NAMES}

REJECTION_DIAGNOSTICS = {
    "missing-import": "cannot snapshot bounded regular source",
    "leaf-import": "leaf imports are unsupported",
    "module-mismatch": "declared module does not match requested module",
    "duplicate-module": "repeated requested observation import module",
    "duplicate-canonical": "repeated canonical observation import source",
    "self-import": "observed root self-import",
    "alias-collision": "duplicate final-segment observation import alias",
    "explicit-alias": "explicit import alias syntax",
    **{name: "call must resolve unambiguously" for name in ["private-qualified-call", "private-root-fallback", "receiver-local-collision", "receiver-root-collision"]},
    **{name: "duplicate scope declaration or emitted source symbol collision" for name in NO_LAUNCH_NAMES},
    "identifier-truncation": "identifier exceeds 127 bytes",
    "dotted-truncation": "complete dotted module spelling exceeds 255 bytes",
    "label-truncation": "test label exceeds 4096 bytes",
    "imported-header": "imported file-wide header",
    "imported-macro": "libraries, C providers and macros",
    "inactive-import": "imports must be unconditional plain root declarations",
    "inactive-leaf-import": "leaf imports are unsupported",
    "syntax-in-leaf": "expected",
    **{name: "cannot snapshot bounded regular source" for name in ["nul-source", "physical-line-limit", "snapshot-byte-limit"]},
    "test-limit": "test inventory capacity",
    "source-file-limit": "observation graph exceeds 256 files or 255 imports",
}
assert set(REJECTION_DIAGNOSTICS) == set(REJECTIONS)

POSITIVE_NAMES = ["basic87", "basic88", "imported-leak13", "imported-leak12",
                  "same-line-files", "global-sibling-alias", "empty-imported-test",
                  "same-module-text", "symlink-requested-directory", "imported-main", "mixed-owner-line"]


def expected_cases(levels, stage="all"):
    inventory = {}
    if stage in ("all", "rejections"):
        inventory["rejection_expected"] = ["reject-" + name for name in BENIGN_REJECTIONS]
    if stage == "compile-only":
        inventory["compile_only"] = ["reject-" + name for name in REJECTIONS]
    if stage in ("all", "native"):
        inventory["observed_native"] = [f"{name}-O{opt}" for opt in levels for name in POSITIVE_NAMES]
    return inventory


def write_sources(folder, sources):
    for name, text in sources.items():
        if name.startswith("@symlink:"): continue
        path = folder / name; path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
    for name, target in sources.items():
        if name.startswith("@symlink:"):
            path = folder / name.removeprefix("@symlink:"); path.parent.mkdir(parents=True, exist_ok=True)
            path.symlink_to(target)
    return folder / "root.sts"


def positive_sources():
    for name, digest in BASIC_HASHES.items():
        assert sha(BASIC / name) == digest, "literal basic fixture changed: " + name
    basic = {name: (BASIC / name).read_text() for name in BASIC_HASHES}
    leak = {"root.sts": "mod root;\nimp dep;\ntest 'root' { expect.(true); }\n",
            "dep.sts": "mod dep;\ntest 'retained imported allocation' {\n    heap u8 *rw p = new.(13);\n    expect.(p != nil);\n}\n"}
    return {
        "imported-main": {"root.sts": "mod root;\nimp dep;\nfn main(void): i32 { ret 99; }\ntest 'imported main' { expect_eq.(dep.main(), 7); }\n",
                          "dep.sts": "mod dep;\next fn main(void): i32 { ret 7; }\n"},
        "mixed-owner-line": {"root.sts": "mod root;\nimp dep; fn never(void): i32 { ret 9; } test 'mixed owner' { expect_eq.(dep.work(), 7); }\n",
                             "dep.sts": "mod dep;\next fn work(void): i32 { ret 7; }\n"},
        "basic87": basic, "basic88": basic,
        "imported-leak13": leak, "imported-leak12": leak,
        "same-line-files": {"root.sts": "mod root;\nimp dep; fn work(void): i32 { ret 100; }\ntest 'same' { expect_eq.(work(), 100); expect_eq.(dep.work(), 7); }\n",
                            "dep.sts": "mod dep;\next fn work(void): i32 { ret 7; }\ntest 'same' { expect_eq.(work(), 7); }\n"},
        "global-sibling-alias": {"root.sts": "mod root;\nimp left;\nimp right;\ntest 'root' { expect_eq.(left.work(), 11); }\n",
                                 "left.sts": "mod left;\next fn work(void): i32 { ret right.work() + 1; }\ntest 'left' { expect_eq.(work(), 11); }\n",
                                 "right.sts": "mod right;\next fn work(void): i32 { ret 10; }\ntest 'right' { expect_eq.(work(), 10); }\n"},
        "empty-imported-test": {"root.sts": "mod root;\nimp dep;\nfn work(void): i32 { ret 7; }\ntest 'root' { expect_eq.(work(), 7); }\n",
                                "dep.sts": "mod dep;\ntest 'empty imported' { }\n"},
        "same-module-text": {"root.sts": "mod dep;\nimp dep;\nfn work(void): i32 { ret 100; }\ntest 'same' { expect_eq.(work(), 100); expect_eq.(dep.work(), 7); }\n",
                             "dep.sts": "mod dep;\next fn work(void): i32 { ret 7; }\ntest 'same' { expect_eq.(work(), 7); }\n"},
        "symlink-requested-directory": {"target/root.sts": "mod root;\nimp dep;\ntest 'requested directory' { expect_eq.(dep.work(), 7); }\n",
                                        "target/dep.sts": "mod dep;\next fn work(void): i32 { ret 99; }\n",
                                        "entry/dep.sts": "mod dep;\next fn work(void): i32 { ret 7; }\n",
                                        "@symlink:entry/root.sts": "../target/root.sts"},
    }


class Canonical:
    """Independent schema implementation: no native structs/JSON ordering."""
    def __init__(self): self.value = bytearray()
    def u32(self, value): self.value.extend(struct.pack(">I", value))
    def u64(self, value): self.value.extend(struct.pack(">Q", value))
    def raw(self, value): self.value.extend(value)
    def blob(self, value): self.u64(len(value)); self.raw(value)
    def text(self, value): self.blob(value.encode("ascii"))
    def optional_blob(self, value):
        self.u32(value is not None)
        if value is not None: self.blob(value)
    def optional_u32(self, value):
        self.u32(value is not None)
        if value is not None: self.u32(value)
    def digest(self): return hashlib.sha256(self.value).hexdigest()


def raw_field(item, name):
    value = item[name + "_bytes_hex"]
    return bytes.fromhex(value) if value is not None else None


def recompute_graph(data):
    out = Canonical(); out.text("stasha-observation-input-graph-v2"); out.u32(2)
    for value in [GRAPH_PROFILE, "requested-entry-directory-v1", "reject-v1", "global-unique-exported-target-v1"]: out.text(value)
    out.blob(raw_field(data, "invocation_directory")); out.blob(raw_field(data, "original_root_spelling")); out.u32(0)
    out.u32(len(data["files"]))
    for file in data["files"]:
        out.u32(file["id"]); out.u32(file["role"])
        out.blob(raw_field(file, "lookup_path")); out.blob(raw_field(file, "canonical_path"))
        out.optional_blob(raw_field(file, "requested_module")); out.blob(raw_field(file, "declared_module"))
        out.u64(file["source_bytes"]); out.u32(file["physical_line_count"]); out.raw(bytes.fromhex(file["source_sha256"]))
    out.u32(len(data["edges"]))
    for edge in data["edges"]:
        for key in ["parent_file_id", "child_file_id", "line", "col", "ordinal"]: out.u32(edge[key])
        out.blob(raw_field(edge, "requested_module")); out.blob(raw_field(edge, "lookup_path"))
    return out.digest()


def recompute_map(data, *, version=4):
    assert version in (3, 4)  # v3 exists only for the preserved pure known answer
    out = Canonical(); out.text("stasha-observation-map-v" + str(version)); out.u32(version)
    out.raw(bytes.fromhex(data["input_graph_sha256"])); out.raw(bytes.fromhex(data["config_sha256"]))
    out.u32(len(data["files"]))
    for file in data["files"]:
        for key in ["id", "role", "physical_line_count"]: out.u32(file[key])
        out.raw(bytes.fromhex(file["source_sha256"]))
    out.u32(len(data["declarations"]))
    for item in data["declarations"]:
        for key in ["id", "file_id", "ordinal_in_file", "kind", "line", "col", "end_line", "end_col"]: out.u32(item[key])
        out.blob(raw_field(item, "module_name")); out.blob(raw_field(item, "name"))
        out.u32(item["source_linkage"]); out.u32(item["selected"])
    out.u32(len(data["tests"]))
    for item in data["tests"]:
        for key in ["id", "decl_id", "file_id", "line", "col"]: out.u32(item[key])
        out.blob(raw_field(item, "name"))
    for table, optional in [("anchors", "coverage_id"), ("sites", "static_test_id")]:
        out.u32(len(data[table]))
        for item in data[table]:
            for key in ["id", "decl_id", "file_id", "line", "col", "end_line", "end_col", "kind"]: out.u32(item[key])
            if table == "anchors": out.u32(item["exclusion_reason"])
            out.optional_u32(item[optional])
            if table == "sites":
                for key in ("slice_element_kind", "slice_abi_stride", "slice_count_arity"): out.u32(item[key])
                if version == 4: out.u32(item["slice_initializer_item_count"])
    out.u32(len(data["coverage"]))
    for item in data["coverage"]:
        for key in ["id", "file_id", "line"]: out.u32(item[key])
    out.u32(len(data["files"]))
    for file in data["files"]:
        out.u32(file["id"]); out.u32(file["physical_line_count"])
        for line, reason in enumerate(file["exclusions"], 1): out.u32(line); out.u32(reason)
    return out.digest()


def strict_json(value):
    def pairs(items):
        result = {}
        for key, item in items:
            assert key not in result, 'duplicate JSON field: ' + key
            result[key] = item
        return result
    def invalid(value): raise ValueError('nonfinite JSON number: ' + value)
    return json.loads(value, object_pairs_hook=pairs, parse_constant=invalid)


def config_digest(compiler, runtime, optimization, *, enabled=3, warn=1, maximum=8, minimum=90,
                  policy='closed-direct-source-primitive-slice-count-or-positional-v1', debug=0, target='x86_64-unknown-linux-gnu', _version=4):
    """Independent literal schema 4; v3 is only a preserved pure KAT."""
    assert _version in (3, 4)
    profile = PROFILE if _version == 4 else 'native-sequential-direct-source-slice-v3'
    resource = 'direct-source-positional-slice-bounds-v4' if _version == 4 else 'direct-source-slice-bounds-v3'
    if _version == 3: policy = 'closed-direct-source-array-and-local-primitive-slices-v1'
    out = Canonical(); out.text('stasha-observation-config-v' + str(_version)); out.u32(_version); out.u32(_version)
    out.text(profile); out.u32(2); out.u32(2)
    for value in (resource, 'requested-entry-directory-v1', 'reject-v1', 'global-unique-exported-target-v1', policy): out.text(value)
    limits = (256,255,16777216,1000000,127,255,511,511,1023,4095,4096,65536,65536,4096,65536,65536,16384,2097152,33554432,33554432,65536,65536,65536,1024)
    if _version == 4: limits += (65536, 65536)
    out.u32(len(limits))
    for value in limits: out.u32(value)
    out.text(target); out.raw(bytes.fromhex(compiler)); out.raw(bytes.fromhex(runtime))
    for value in (optimization, debug, enabled, warn, maximum is not None): out.u32(value)
    if maximum is not None: out.u64(maximum)
    out.optional_u32(minimum)
    return out.digest(), bytes(out.value)


def map_known_answer():
    def record(keys, values): return dict(zip(keys.split(), values))
    declarations = []
    for fields, name in [([0,0,0,1,1,1,3,1], b'f'), ([1,0,1,58,4,1,6,1], b'')]:
        declarations.append({**record('id file_id ordinal_in_file kind line col end_line end_col', fields),
            'module_name_bytes_hex': b'root'.hex(), 'name_bytes_hex': name.hex(), 'source_linkage': 0, 'selected': 1})
    site_keys = 'id decl_id file_id line col end_line end_col kind static_test_id slice_element_kind slice_abi_stride slice_count_arity slice_initializer_item_count'
    return {'input_graph_sha256': bytes(range(32)).hex(), 'config_sha256': bytes(range(32,64)).hex(),
        'files': [dict(id=0,role=0,physical_line_count=6,source_sha256='a5'*32,exclusions=[2,1,2,4,4,4])],
        'declarations': declarations, 'tests': [dict(id=0,decl_id=1,file_id=0,line=4,col=1,name_bytes_hex=b't'.hex())],
        'anchors': [record('id decl_id file_id line col end_line end_col kind exclusion_reason coverage_id', fields) for fields in
                    ([0,0,0,2,1,2,80,1,0,0], [1,1,0,5,1,5,80,3,2,None])],
        'sites': [record(site_keys, fields) for fields in
                  ([0,0,0,2,1,2,10,1,None,0,0,0,0], [1,0,0,2,12,2,22,2,None,0,0,0,0],
                   [2,0,0,2,24,2,30,3,None,0,0,0,0], [3,0,0,2,32,2,50,4,None,5,4,2,0],
                   [4,1,0,5,1,5,20,4,0,9,1,1,0], [5,1,0,5,22,5,30,5,0,0,0,0,0])],
        'coverage': [dict(id=0,file_id=0,line=2)]}


def validate_site_metadata(sites, test_count):
    """Closed v4 constructor metadata, independent of any native allocation."""
    strides = (0,1,1,2,2,4,4,8,8,1,4,8)
    total = 0
    for site in sites:
        def number(key, maximum):
            value = site[key]
            assert type(value) is int and 0 <= value <= maximum, (key, value)
            return value
        kind = number('kind', 6); assert kind >= 1
        element = number('slice_element_kind', 11)
        stride = number('slice_abi_stride', 8)
        arity = number('slice_count_arity', 2)
        count = number('slice_initializer_item_count', 65536)
        if kind == 4: assert element > 0 and stride == strides[element] and arity in (1,2) and count == 0
        elif kind == 6: assert element > 0 and stride == strides[element] and arity == 0
        else: assert element == stride == arity == count == 0
        total += count; assert total <= 65536
        owner = site['static_test_id']
        if owner is not None: assert type(owner) is int and 0 <= owner < test_count
        if 'owner_test' in site: assert site['owner_test'] == (4294967295 if owner is None else owner)


def validate_raw_report(data, report, *, require_complete=True):
    """Exhaustive ordered wire grammar plus independently checked domain ledger.

    Reconstruct every canonical raw row from JSON only after checking all numeric
    types, IDs, domain/site semantics, test lifecycle, generations and totals.
    Extra/missing/reordered/duplicate/trailing raw text cannot be ignored.
    """
    u64max, u32max = 2**64 - 1, 2**32 - 1
    def number(value, maximum=u64max):
        assert type(value) is int and 0 <= value <= maximum, ('invalid unsigned field', value)
        return value
    assert type(report) is str and report.isascii() and len(report.encode('ascii')) <= 2097152
    assert data['schema'] == 4 and data['profile'] == PROFILE
    assert data['runtime_abi'] == 4 and data['allocation_metric_version'] == data['line_metric_version'] == 2
    assert data['report_valid'] is True
    complete, reason, endpoint = number(data['complete'],1), number(data['reason'],14), number(data['suite_endpoint'],1)
    if complete: assert reason == 0 and endpoint == 1
    else: assert reason != 0 and endpoint == 0
    if require_complete: assert complete == 1
    tests, sites, coverage = data['tests'], data['sites'], data['coverage']
    assert 1 <= len(tests) <= 4096 and len(sites) <= 65536 and len(coverage) <= 65536
    started, completed = number(data['tests_started'], len(tests)), number(data['tests_completed'], len(tests))
    assert completed <= started and started - completed <= 1
    if complete: assert started == completed == len(tests)
    assert [number(t['id'], 4095) for t in tests] == list(range(len(tests)))
    assert [number(s['id'], 65535) for s in sites] == list(range(len(sites)))
    assert [number(c['id'], 65535) for c in coverage] == list(range(len(coverage)))
    assert len({(c['file_id'], c['line']) for c in coverage}) == len(coverage)
    assert re.fullmatch(r'x86_64-(?:unknown|pc)-linux-gnu', data['target'])
    validate_site_metadata(sites, len(tests))
    previous, live_bytes = 0, 0
    domains = {'explicit_user_heap': dict(live_allocations=0,tracked_outstanding_bytes=0),
               'generated_primitive_slice_backing': dict(live_allocations=0,tracked_outstanding_bytes=0)}
    records = data['allocations']; assert len(records) <= 16384
    rows = ['STASHA_OBSERVATION 4', 'profile ' + PROFILE, 'runtime_abi 4', 'metric_versions 2 2']
    for raw, key in [('run','run'),('input_graph','input_graph_sha256'),('map','map_sha256'),('config','config_sha256'),('compiler','compiler_sha256')]:
        value = data[key]; assert type(value) is str and re.fullmatch(r'[0-9a-f]{64}', value)
        rows.append(raw + ' ' + value)
    enabled = number(data['enabled'], 3); assert enabled
    passed, failed = number(data['assertions_passed']), number(data['assertions_failed']); assert passed <= u64max-failed
    if not started: assert passed == failed == 0, 'assertion before any test began'
    rows += [f'enabled {enabled}', f'inventory {len(tests)} {len(coverage)} {len(sites)}', f'state {complete} {reason} {endpoint}',
             f'assertions {passed} {failed}', f'tests {started} {completed}']
    rows += [f'test {i} {2 if i < completed else 1 if i < started else 0}' for i in range(len(tests))]
    bitmap = bytearray((len(coverage)+7)//8)
    assert [f['id'] for f in data['files']] == list(range(len(data['files'])))
    hit_pairs = set()
    for file in data['files']:
        assert len(file['hit_lines']) == len(set(file['hit_lines']))
        assert set(file['hit_lines']) <= set(file['lines'])
        assert file['covered_lines'] == len(file['hit_lines'])
        hit_pairs.update((file['id'], line) for line in file['hit_lines'])
    assert hit_pairs <= {(c['file_id'],c['line']) for c in coverage}
    for i, point in enumerate(coverage):
        is_hit = (point['file_id'],point['line']) in hit_pairs
        assert type(point['hit']) is bool and point['hit'] == is_hit
        if is_hit: bitmap[i//8] |= 1 << (i%8)
    assert data['covered_lines'] == len(hit_pairs)
    if not enabled & 2: assert not hit_pairs
    if not started: assert not hit_pairs, 'hit before any test began'
    rows.append('hits ' + (bitmap.hex() or '-'))
    allocation_rows=[]
    for record in records:
        assert set(record) == {'generation','domain','bytes','allocation_site','resize_site','test'}
        generation=number(record['generation']); assert previous < generation; previous=generation
        domain=number(record['domain'],2); assert domain in (1,2)
        size=number(record['bytes']); origin=number(record['allocation_site'],len(sites)-1)
        resize=number(record['resize_site'],u32max); test=number(record['test'],len(tests)-1)
        assert test < started, 'allocation attributed to an unstarted test'
        site=sites[origin]
        assert site['static_test_id'] is None or site['static_test_id']==test
        if domain==1:
            assert site['kind'] in (1,2)
            if resize!=u32max: assert resize<len(sites) and sites[resize]['kind']==2
            key='explicit_user_heap'
        else:
            assert site['kind'] in (4,6) and resize==u32max and size>0
            stride=site['slice_abi_stride']; assert size%stride==0 and size//stride<=2147483647
            if site['kind'] == 6: assert size == site['slice_initializer_item_count'] * stride
            key='generated_primitive_slice_backing'
        assert live_bytes <= u64max-size; live_bytes+=size
        subtotal=domains[key]; assert subtotal['tracked_outstanding_bytes']<=u64max-size
        subtotal['tracked_outstanding_bytes']+=size; subtotal['live_allocations']+=1
        allocation_rows.append('allocation '+' '.join(str(record[k]) for k in ('generation','domain','bytes','allocation_site','resize_site','test')))
    assert number(data['live_allocations'],16384)==len(records)
    assert number(data['tracked_outstanding_bytes'])==live_bytes
    assert set(data['allocation_domains']) == set(domains)
    for value in data['allocation_domains'].values():
        assert set(value) == {'live_allocations','tracked_outstanding_bytes'}
        number(value['live_allocations'],16384); number(value['tracked_outstanding_bytes'])
    assert data['allocation_domains']==domains
    rows += [f'allocations {len(records)} {live_bytes}', *allocation_rows, 'END']
    assert report == '\n'.join(rows)+'\n', 'noncanonical, mismatched, incomplete or trailing report data'
    return rows


def raw_report_known_answer():
    # Standalone complete model using only literal protocol inputs, not output.
    model = map_known_answer()
    model.update(schema=4, profile=PROFILE, runtime_abi=4, allocation_metric_version=2, line_metric_version=2,
        target='x86_64-unknown-linux-gnu', report_valid=True, complete=1, reason=0, suite_endpoint=1,
        tests_started=1, tests_completed=1, enabled=3, assertions_passed=2, assertions_failed=0,
        covered_lines=1, live_allocations=2, tracked_outstanding_bytes=27, run='11'*32, compiler_sha256='dd'*32,
        map_sha256='33'*32, allocations=[dict(generation=1,domain=1,bytes=7,allocation_site=0,resize_site=4294967295,test=0),
                                      dict(generation=2,domain=2,bytes=20,allocation_site=3,resize_site=4294967295,test=0)],
        allocation_domains={'explicit_user_heap':dict(live_allocations=1,tracked_outstanding_bytes=7),
                            'generated_primitive_slice_backing':dict(live_allocations=1,tracked_outstanding_bytes=20)})
    model['files'][0].update(lines=[2],hit_lines=[2],covered_lines=1)
    model['coverage'][0]['hit'] = True
    report = ('STASHA_OBSERVATION 4\nprofile native-sequential-direct-source-positional-slice-v4\nruntime_abi 4\nmetric_versions 2 2\n'
        + 'run ' + '11'*32 + '\ninput_graph ' + bytes(range(32)).hex() + '\nmap ' + '33'*32
        + '\nconfig ' + bytes(range(32,64)).hex() + '\ncompiler ' + 'dd'*32
        + '\nenabled 3\ninventory 1 1 6\nstate 1 0 1\nassertions 2 0\ntests 1 1\ntest 0 2\nhits 01\n'
        + 'allocations 2 27\nallocation 1 1 7 0 4294967295 0\nallocation 2 2 20 3 4294967295 0\nEND\n')
    return model, report


def validate_driver(data, report, session, binary, entry, *, expected, pairs, hits, assertions, tests, live=0, byte_count=0):
    assert data["schema"] == 4 and data["profile"] == PROFILE
    assert re.fullmatch(r"[0-9a-f]{64}", data["input_graph_sha256"])
    assert recompute_graph(data) == data["input_graph_sha256"]
    assert recompute_map(data) == data["map_sha256"]
    assert data["compiler_sha256"] == session.compiler_hash
    assert data["runtime_sha256"] == session.runtime_hash
    assert data["binary_sha256"] == sha(binary)
    assert data["report_valid"] and data["complete"] == 1 and data["suite_endpoint"] == 1
    assert data["compile_link_success"] and data["manifest_valid"] and data["overall_status"] == expected
    child = data["child"]
    assert child["launched"] and child["waited"] and child["exited"] and child["exit_code"] == 0
    assert child["signal"] == child["launch_errno"] == child["wait_errno"] == 0
    assert (data["assertions_passed"], data["assertions_failed"]) == (assertions, 0)
    assert (data["live_allocations"], data["tracked_outstanding_bytes"]) == (live, byte_count)
    assert len(data["tests"]) == tests
    assert data["tests_started"] == data["tests_completed"] == tests
    coverage = data["coverage"]
    assert [(item["file_id"], item["line"]) for item in coverage] == pairs
    assert [item["id"] for item in coverage] == list(range(len(pairs)))
    rows = validate_raw_report(data, report)
    bitmap = next(row.split(" ")[1] for row in rows if row.startswith("hits "))
    raw = bytes.fromhex(bitmap) if bitmap != "-" else b""
    assert len(raw) == (len(pairs) + 7) // 8
    expected_raw = bytearray(len(raw))
    for index, pair in enumerate(pairs):
        if pair in hits: expected_raw[index // 8] |= 1 << (index % 8)
    assert raw == expected_raw, (raw.hex(), expected_raw.hex())
    assert data["covered_lines"] == len(hits)
    if data["enabled"] & 2:
        passes = bool(pairs) and len(hits) * 100 >= data["min_test_coverage"] * len(pairs)
        assert data["coverage_metric"] == ("pass" if passes else "fail")
    else:
        assert data["coverage_metric"] == "disabled"
    maximum = data["max_leaked_bytes"]
    assert data["allocation_metric"] == ("disabled" if not data["enabled"] & 1 else "pass" if maximum is None or byte_count <= maximum else "fail")
    files = data["files"]
    assert [item["id"] for item in files] == list(range(len(files)))
    assert [item["role"] for item in files] == [0] + [1] * (len(files) - 1)
    assert files[0]["source_sha256"] == sha(entry)
    for file in files:
        lookup = os.fsdecode(bytes.fromhex(file["lookup_path_bytes_hex"]))
        canonical = os.fsdecode(bytes.fromhex(file["canonical_path_bytes_hex"]))
        assert Path(lookup).resolve() == Path(canonical)
        assert sha(lookup) == file["source_sha256"]
        assert file["physical_line_count"] == 1 + Path(lookup).read_bytes().count(b"\n")
        eligible = [line for owner, line in pairs if owner == file["id"]]
        hit = [line for owner, line in hits if owner == file["id"]]
        assert file["lines"] == eligible and file["eligible_lines"] == len(eligible) and file["hit_lines"] == hit and file["covered_lines"] == len(hit)
        assert len(file["exclusions"]) == file["physical_line_count"]
    assert len(data["edges"]) == len(files) - 1
    assert all(edge["parent_file_id"] == 0 and edge["child_file_id"] == i + 1 for i, edge in enumerate(data["edges"]))
    for table in (data["declarations"], data["anchors"]):
        assert [item["id"] for item in table] == list(range(len(table)))
        assert all(item["file_id"] < len(files) for item in table)
    return raw


def run_native(session, sources, levels):
    seen_runs, seen_evidence = set(), set()
    signatures = {}
    oracles = {
        "imported-main": ([(1, 2)], [(1, 2)], 1, 1, 0, 0, 100, 0),
        "mixed-owner-line": ([(0, 2), (1, 2)], [(1, 2)], 1, 1, 0, 0, 50, 0),
        "basic87": (BASIC_LINES, BASIC_HITS, 5, 2, 0, 0, 87, 0),
        "basic88": (BASIC_LINES, BASIC_HITS, 5, 2, 0, 0, 88, 1),
        "imported-leak13": ([], [], 2, 2, 1, 13, None, 0),
        "imported-leak12": ([], [], 2, 2, 1, 13, None, 1),
        "same-line-files": ([(0, 2), (1, 2)], [(0, 2), (1, 2)], 3, 2, 0, 0, 100, 0),
        "global-sibling-alias": ([(1, 2), (2, 2)], [(1, 2), (2, 2)], 3, 3, 0, 0, 100, 0),
        "empty-imported-test": ([(0, 3)], [(0, 3)], 1, 2, 0, 0, 100, 0),
        "same-module-text": ([(0, 3), (1, 2)], [(0, 3), (1, 2)], 3, 2, 0, 0, 100, 0),
        "symlink-requested-directory": ([(1, 2)], [(1, 2)], 1, 1, 0, 0, 100, 0),
    }
    assert set(oracles) == set(POSITIVE_NAMES)
    for opt in levels:
        for name in POSITIVE_NAMES:
            entry = sources[name]
            pairs, hits, assertions, tests, live, byte_count, minimum, expected = oracles[name]
            key = f"{name}-O{opt}"
            maximum = 12 if name == "imported-leak12" else byte_count
            flags = ["--max-leaked-bytes", str(maximum)]
            if minimum is not None: flags += ["--min-test-coverage", str(minimum)]
            binary = session.artifacts / (key + ".test")
            env = dict(os.environ)
            pre, post = session.artifacts / (key + ".pre.ll"), session.artifacts / (key + ".post.ll")
            env.update(STS_DUMP_IR_PRE=str(pre), STS_DUMP_IR=str(post))
            dest, result = session.command(key, [session.compiler, "test", entry, f"-o={opt}", "-o", binary, *flags], expected=expected, env=env)
            match = re.search(rb"observation: private evidence ([^\r\n]+)", result.stderr)
            assert match, key + ": missing driver evidence"
            evidence = Path(os.fsdecode(match.group(1)))
            assert str(evidence.resolve()) not in seen_evidence and not evidence.is_symlink()
            seen_evidence.add(str(evidence.resolve()))
            metadata = (evidence / "driver.json").read_bytes(); report = (evidence / "report").read_bytes()
            assert len(metadata) <= 32 * 1024 * 1024 and len(report) <= 2 * 1024 * 1024
            (dest / "driver.json").write_bytes(metadata); (dest / "report").write_bytes(report)
            data = strict_json(metadata)
            assert data["run"] not in seen_runs, "stale run ID"; seen_runs.add(data["run"])
            raw = validate_driver(data, report.decode("ascii"), session, binary, entry,
                                  expected=expected, pairs=pairs, hits=hits, assertions=assertions,
                                  tests=tests, live=live, byte_count=byte_count)
            for path in (pre, post):
                ir = path.read_text()
                assert "__stasha_obs_init_v4" in ir and not re.search(r"@__stasha_obs_init\(", ir)
            if name.startswith("basic"):
                assert raw == b"\x7f"
                assert [(item["id"], item["file_id"], item["ordinal_in_file"], item["kind"], item["line"]) for item in data["declarations"]] == [
                    (0, 0, 0, 6, 2), (1, 0, 1, 1, 3), (2, 0, 2, 58, 6),
                    (3, 1, 0, 2, 2), (4, 1, 1, 1, 3), (5, 1, 2, 1, 4), (6, 1, 3, 1, 11), (7, 1, 4, 58, 14)]
                assert [test["decl_id"] for test in data["tests"]] == [2, 7]
                assert [site["decl_id"] for site in data["sites"]] == [5, 5]
                assert [file["physical_line_count"] for file in data["files"]] == [11, 17]
                assert [(test["id"], test["file_id"], test["line"], test["name"]) for test in data["tests"]] == [(0, 0, 6, "same label"), (1, 1, 14, "same label")]
                assert [(site["id"], site["file_id"], site["line"], site["kind"], site["static_test_id"]) for site in data["sites"]] == [(0, 1, 5, 1, None), (1, 1, 7, 3, None)]
            if name.startswith("imported-leak"):
                assert [(site["id"], site["file_id"], site["line"], site["kind"], site["static_test_id"]) for site in data["sites"]] == [(0, 1, 3, 1, 1)]
                records = [row.split() for row in report.decode("ascii").splitlines() if row.startswith("allocation ")]
                assert len(records) == 1 and records[0][2:] == ["1", "13", "0", "4294967295", "1"]
            enabled = 1 | (2 if minimum is not None else 0)
            assert data['enabled'] == enabled and data['warn_memleak'] is False
            assert data['max_leaked_bytes'] == maximum and data['min_test_coverage'] == minimum
            assert data['config_sha256'] == config_digest(session.compiler_hash, session.runtime_hash, opt,
                enabled=enabled, warn=0, maximum=maximum, minimum=minimum, target=data['target'])[0]
            signature = (data["coverage"], [(file["lines"], file["exclusions"]) for file in data["files"]], data["tests"], data["sites"])
            if name in signatures: assert signature == signatures[name], name + ": optimization changed source inventory"
            signatures[name] = signature
            session.cases.append({"name": key, "stage": "observed_native", "status": "pass", "executions": 1,
                                  "input_graph_sha256": data["input_graph_sha256"], "map_sha256": data["map_sha256"],
                                  "config_sha256": data["config_sha256"], "binary_sha256": sha(binary), "run_id": data["run"],
                                  "pre_ir_sha256": sha(pre), "post_ir_sha256": sha(post),
                                  "assertions_passed": assertions, "tests": tests,
                                  "eligible": len(pairs), "covered": len(hits), "live_allocations": live,
                                  "tracked_outstanding_bytes": byte_count})


def compile_only_inventory(levels):
    leaf = load_module("leaf_nonlaunch_inventory", ROOT / "tests/observation/test_observation.py")
    ordinary = load_module("ordinary_nonlaunch_inventory", ROOT / "tests/scalars/test_imported_tests.py")
    return {"compile_only": ["reject-" + name for name in REJECTIONS] +
            ["leaf-reject-" + name for name in leaf.REJECTIONS] + ["leaf-raw-call-fixture"] +
            [f"ir-{name}-O{opt}" for opt in levels for name in POSITIVE_NAMES] +
            [f"leaf-ir-{name}-O{opt}" for opt in levels for name in leaf.COMPILE_ONLY_CONTROLS] +
            [f"ordinary-ir-{name}-O{opt}" for opt in levels for name in ordinary.CASES]}


def validate_leaf_rejection_diagnostic(name, stderr):
    # This one preserved v1 source is now a missing supported plain import.
    # Its rejection remains mandatory, with the exact missing-module identity.
    if name == "import":
        assert b"cannot snapshot bounded regular source" in stderr and b"deliberately_missing.sts" in stderr, (name, stderr)
    else:
        assert b"unsupported by observation profile" in stderr, (name, stderr)


POSITIVE_NONLAUNCH_FAILURES = re.compile(
    rb"llvm[^\r\n]*(?:verif|error|invalid)|verification[^\r\n]*(?:fail|error)|"
    rb"code[ _-]*(?:gen(?:eration)?)[^\r\n]*(?:fail|error)|"
    rb"optimi[sz](?:ation|er)[^\r\n]*(?:fail|error)|(?:fail|error)[^\r\n]*optimi[sz]|"
    rb"internal[ _-]*(?:compiler[ _-]*)?error|fatal[ _-]*error|"
    rb"assert(?:ion)?[^\r\n]*(?:fail|abort)|segmentation fault|stack dump|"
    rb"broken (?:module|function)|invalid (?:debug|!dbg)", re.IGNORECASE)


def validate_positive_nonlaunch_result(result, output):
    """IR dumps alone cannot hide a later verifier or code-generation failure."""
    assert result.returncode == 1, "positive nonlaunch case requires the expected linker failure status"
    assert b"hosted Linux linking requires a working native C compiler" in result.stderr, "missing expected nofork linker-discovery failure"
    assert b"linking failed" in result.stderr, "missing expected linking failure"
    assert not POSITIVE_NONLAUNCH_FAILURES.search(result.stdout + b"\n" + result.stderr), "positive nonlaunch case has verifier/code-generation/internal failure"
    assert b"passed, " not in result.stdout, "positive nonlaunch case executed tests"
    assert not output.exists(), "positive nonlaunch case produced a native image"


def retain_nonlaunch_evidence(destination, result):
    """A private evidence directory can exist after rejection or a failed link."""
    match = re.search(rb"observation: private evidence ([^\r\n]+)", result.stderr)
    if not match: return None
    evidence = Path(os.fsdecode(match.group(1)))
    raw = (evidence / "driver.json").read_bytes()
    assert len(raw) <= 32 * 1024 * 1024
    data = json.loads(raw)
    assert data["schema"] == 4 and data["profile"] == PROFILE
    assert data["child"]["launched"] == 0 and data["child"]["exited"] == 0
    assert not data["report_valid"] and data["overall_status"] != 0
    assert not (evidence / "report").exists(), "nonlaunch case produced runtime report"
    (destination / "driver.json").write_bytes(raw)
    return data


def run_rejections(session, sources, *, compiler=None, nonlaunch=False):
    selected = REJECTIONS if nonlaunch else BENIGN_REJECTIONS
    compiler = compiler or session.compiler
    for name in selected:
        key = "reject-" + name
        output = session.artifacts / (key + ".rejected")
        pre = session.artifacts / (key + ".unexpected.pre.ll")
        post = session.artifacts / (key + ".unexpected.post.ll")
        env = dict(os.environ, STS_DUMP_IR_PRE=str(pre), STS_DUMP_IR=str(post))
        dest, result = session.command(key, [compiler, "test", sources[name], "--warn-memleak", "-o", output], expected=1, env=env)
        assert b"REJECT_IMAGE_MUST_NOT_RUN\n" not in result.stdout, key + ": invalid image executed"
        retain_nonlaunch_evidence(dest, result)
        assert not pre.exists() and not post.exists(), key + ": rejected source reached LLVM output"
        assert not output.exists(), key + ": rejection produced an image"
        assert REJECTION_DIAGNOSTICS[name].encode() in result.stderr, (key, "wrong rejection diagnostic", result.stderr)
        if name in {"leaf-import", "identifier-truncation", "label-truncation", "imported-header", "imported-macro", "inactive-leaf-import", "syntax-in-leaf", "private-root-fallback"}:
            assert b"dep.sts" in result.stderr, key + ": missing owning imported-file diagnostic"
            assert b"import" in result.stderr.lower(), key + ": missing import context"
        session.cases.append({"name": key, "stage": "compile_only" if nonlaunch else "rejection_expected", "status": "pass",
                              "executions": 0, "possibly_executed_on_erroneous_acceptance": not nonlaunch})


def run_compile_only(session, positive, negative, ordinary_sources, levels, ir_compiler):
    """A separately reviewed exact-object --wrap=fork image is supplied, not built here."""
    ir_compiler = ir_compiler.resolve()
    assert ir_compiler != session.compiler and sha(ir_compiler) != session.compiler_hash, "nonlaunch compiler must be separately identified"
    digest = sha(ir_compiler)
    session.ir_compiler_hash = digest
    leaf = load_module("leaf_nonlaunch_controls", ROOT / "tests/observation/test_observation.py")
    run_rejections(session, negative, compiler=ir_compiler, nonlaunch=True)
    for name, body in leaf.REJECTIONS.items():
        key = "leaf-reject-" + name
        source = session.artifacts / "sources/leaf-rejections" / (name + ".sts")
        output = session.artifacts / (key + ".rejected")
        dest, result = session.command(key, [ir_compiler, "test", source, "--warn-memleak", "-o", output], expected=1)
        validate_leaf_rejection_diagnostic(name, result.stderr)
        assert b"UNSUPPORTED_RAN\n" not in result.stdout
        retain_nonlaunch_evidence(dest, result)
        assert not output.exists()
        session.cases.append({"name": key, "stage": "compile_only", "status": "pass", "executions": 0})
    raw_dest, raw_result = session.command("leaf-raw-call-fixture", [ir_compiler, "test", ROOT / "tests/observation/fixtures/unsupported.sts",
                                             "--warn-memleak", "-o", session.artifacts / "leaf-raw.rejected"], expected=1)
    retain_nonlaunch_evidence(raw_dest, raw_result)
    assert b"unsupported by observation profile" in raw_result.stderr
    session.cases.append({"name": "leaf-raw-call-fixture", "stage": "compile_only", "status": "pass", "executions": 0})
    for opt in levels:
        controls = [("ir-" + name, source, []) for name, source in positive.items()]
        controls += [("leaf-ir-" + name, session.artifacts / "sources/leaf-controls" / (name + ".sts"), control["pre_ir_patterns"])
                     for name, control in leaf.COMPILE_ONLY_CONTROLS.items()]
        for prefix, source, patterns in controls:
            key = f"{prefix}-O{opt}"
            pre, post = session.artifacts / (key + ".pre.ll"), session.artifacts / (key + ".post.ll")
            env = dict(os.environ, STS_DUMP_IR_PRE=str(pre), STS_DUMP_IR=str(post))
            output = session.artifacts / (key + ".not-launched")
            dest, result = session.command(key, [ir_compiler, "test", source, f"-o={opt}", "--warn-memleak",
                                             "--min-test-coverage", "0", "-o", output], expected=1, env=env)
            validate_positive_nonlaunch_result(result, output)
            retain_nonlaunch_evidence(dest, result)
            assert b"passed, " not in result.stdout
            before, after = pre.read_text(), post.read_text()
            assert "__stasha_obs_init_v4" in before and "__stasha_obs_init_v4" in after
            for pattern in patterns: assert re.search(pattern, before, re.MULTILINE), (key, pattern)
            session.cases.append({"name": key, "stage": "compile_only", "status": "pass", "executions": 0,
                                  "pre_ir_sha256": sha(pre), "post_ir_sha256": sha(post), "independent_ir_review": "required"})
    for opt in levels:
        for name, source in ordinary_sources.items():
            key = f"ordinary-ir-{name}-O{opt}"
            pre, post = session.artifacts / (key + ".pre.ll"), session.artifacts / (key + ".post.ll")
            env = dict(os.environ, STS_DUMP_IR_PRE=str(pre), STS_DUMP_IR=str(post))
            output = session.artifacts / (key + ".not-launched")
            _, result = session.command(key, [ir_compiler, "test", source, f"-o={opt}", "-o", output], expected=1, env=env)
            validate_positive_nonlaunch_result(result, output)
            before, after = pre.read_text(), post.read_text()
            assert "__test_" in before and "@main(" in after
            assert "__stasha_obs_" not in before and "__stasha_obs_" not in after
            assert b"passed, " not in result.stdout
            session.cases.append({"name": key, "stage": "compile_only", "status": "pass", "executions": 0,
                                  "pre_ir_sha256": sha(pre), "post_ir_sha256": sha(post), "independent_ir_review": "required"})
    assert sha(ir_compiler) == digest, "nonlaunch compiler changed"


def enrollment_selftest(artifacts):
    """Exercise real parent dispatch/receipt code; every command is a Python mock."""
    wrapper = load_module("enrollment_wrapper", ROOT / "tests/observation/test_observation.py")
    ordinary = load_module("enrollment_ordinary", ROOT / "tests/scalars/test_imported_tests.py")
    arrays = load_module("enrollment_array_queries", ROOT / "tests/observation/test_array_queries.py")
    slices = load_module("enrollment_primitive_slices", ROOT / "tests/observation/test_primitive_slices.py")
    levels = [0, 1, 2, 3]
    positional = load_module("enrollment_positional_slices", ROOT / "tests/observation/test_positional_slices.py")
    expected_by_family = {"ordinary-imported-tests": {"ordinary_native": ordinary.expected_cases(levels)},
                          "observed-source-imports": expected_cases(levels),
                          "observed-array-queries": arrays.expected_cases(levels),
                          "observed-primitive-slices": slices.expected_cases(levels),
                          "observed-positional-slices": positional.expected_cases(levels)}

    class FakeQualification:
        def __init__(self, work, mutation=None, affected="ordinary-imported-tests"):
            self.artifacts = work; self.compiler = work / "exact-stasha"
            self.compiler_hash = "a" * 64; self.runtime_hash = "b" * 64
            self.source_manifest = {name: sha(ROOT / name) for name in [
                "tests/scalars/test_imported_tests.py", "tests/observation/test_source_imports.py", "tests/observation/test_observation.py", "tests/observation/test_array_queries.py", "tests/observation/test_primitive_slices.py", "tests/observation/test_positional_slices.py", "tests/observation/fixtures/positional_slices/corpus.json"]}
            self.family_receipts, self.records = [], []
            self.timeouts = {}
            self.status = "running"
            self.mutation, self.affected = mutation, affected
        def verify_inputs(self):
            if self.mutation == "changed-parent-source": raise AssertionError("source changed")
        def command(self, name, argv, **kwargs):
            self.records.append((name, argv))
            if name not in expected_by_family: return self.artifacts, subprocess.CompletedProcess(argv, 0, b"", b"")
            self.timeouts[name] = kwargs["timeout"]
            mutation = self.mutation if name == self.affected else None
            if mutation == "nonzero": raise AssertionError("child exit 1")
            if mutation == "timeout": raise subprocess.TimeoutExpired(argv, 120)
            if mutation == "exception": raise OSError("mock child unavailable")
            destination = Path(argv[argv.index("--artifacts") + 1]); destination.mkdir()
            result = destination / "run-mock"; result.mkdir()
            if mutation == "missing-latest": return
            (destination / "LATEST").write_text(str(result) + "\n")
            if mutation == "missing-receipt": return
            inventory = expected_by_family[name]
            runner = FAMILY_IDENTITIES[name][0]
            manifest_path = result / "source-manifest.json"
            manifest_path.write_text(json.dumps(self.source_manifest))
            source_path = result / "mock.sts"; source_path.write_text("mod mock;\n")
            generated = {"mock.sts": sha(source_path)}
            (result / "generated-manifest.json").write_text(json.dumps(generated))
            data = {"schema": 2, "family": name, "status": "pass", "failure": None,
                    "compiler_sha256": self.compiler_hash, "runtime_sha256": self.runtime_hash,
                    "observation_profile": PROFILE if FAMILY_IDENTITIES[name][1] else None,
                    "runner_sha256": self.source_manifest[runner], "optimization_levels": levels,
                    "source_manifest": self.source_manifest, "source_manifest_sha256": sha(manifest_path),
                    "generated_manifest": generated, "generated_resolutions": {"mock.sts": str(source_path.resolve())},
                    "inventory": inventory, "counts": {stage: len(cases) for stage, cases in inventory.items()},
                    "cases": [{"name": case, "stage": stage, "status": "pass", "executions": 1}
                              for stage, cases in inventory.items() for case in cases],
                    "runs": [{"name": "mock-run", "status": "pass"}],
                    "artifact_manifest": {"source-manifest.json": sha(manifest_path), "mock.sts": sha(source_path),
                        "generated-manifest.json": sha(result / "generated-manifest.json")}}
            if name == 'observed-positional-slices':
                data['qualification_contract'] = dict(wrapper.POSITIONAL_QUALIFICATION_CONTRACT)
                commands = [case + suffix for case in inventory['ordinary_native'] for suffix in ('-compile','-execute')] + inventory['observed_native']
                data['runs'] = [dict(name=command,status='pass') for command in commands]
            for case in data["cases"]:
                if case["stage"] == "observed_native": case["run_id"] = hashlib.sha256((name+":"+case["name"]).encode()).hexdigest()
            if mutation == "malformed": (result / "result.json").write_text("{"); return
            if mutation in ("running", "fail", "skipped"): data["status"] = mutation
            if mutation == "compiler": data["compiler_sha256"] = "c" * 64
            if mutation == "runtime": data["runtime_sha256"] = "c" * 64
            if mutation == "profile": data["observation_profile"] = "native-sequential-direct-source-v2"
            if mutation == "family": data["family"] = "substituted-family"
            if mutation == "unexpected-case": data["cases"].append(dict(name="unexpected",stage="ordinary_native",status="pass",executions=1))
            if mutation == "duplicate-run": data["runs"].append(dict(data["runs"][0]))
            if mutation == "duplicate-run-id":
                observed = [case for case in data["cases"] if case["stage"] == "observed_native"]
                observed[1]["run_id"] = observed[0]["run_id"]
            if mutation == "artifact-hash": data["artifact_manifest"]["mock.sts"] = "c" * 64
            if mutation == "artifact-path": data["artifact_manifest"]["../outside"] = "c" * 64
            if mutation == "missing-artifact": data["artifact_manifest"] = {}

            if mutation == "runner": data["runner_sha256"] = "c" * 64
            if mutation == "optimizations": data["optimization_levels"] = [0]
            if mutation == "source": data["source_manifest"] = {}
            if mutation == "missing-generated": source_path.unlink()
            if mutation == "changed-generated": source_path.write_text("changed")
            if mutation == "missing-case": data["cases"].pop()
            if mutation == "duplicate-case": data["cases"].append(data["cases"][0])
            if mutation == "skipped-case": data["cases"][0]["status"] = "skipped"
            if mutation == "count": data["counts"] = {}
            if mutation == "run-status": data["runs"][0]["status"] = "running"
            if mutation == "zero-executions":
                next(case for case in data["cases"] if case["stage"].endswith("native"))["executions"] = 0
            if mutation == "positional-missing-command": data["runs"].pop(99)
            if mutation == "positional-reordered-command": data["runs"][0],data["runs"][1] = data["runs"][1],data["runs"][0]
            if mutation == "positional-contract": data['qualification_contract']['cumulative_timeout_seconds'] = 7200
            if mutation == "positional-barrier":
                data['cases'] = data['cases'][184:185] + data['cases'][:184] + data['cases'][185:]
            if mutation == "outside-receipt": (destination / "LATEST").write_text(str(self.artifacts) + "\n")
            (result / "result.json").write_text(json.dumps(data))

    class EnrollmentTests(unittest.TestCase):
        def test_success_exact_enrollment(self):
            with tempfile.TemporaryDirectory() as temp:
                q = FakeQualification(Path(temp))
                wrapper.pure_components(q, "selected-clang --target=native")
                wrapper.dispatch_required_families(q, levels, "selected-clang --target=native")
                names = [name for name, _ in q.records]
                self.assertEqual(names, ["runtime-unit-build", "runtime-unit-run", "driver-unit-build", "driver-unit-run",
                                         "source-graph-unit-build", "source-graph-unit-run", "ordinary-imported-tests", "observed-source-imports", "observed-array-queries", "observed-primitive-slices", "observed-positional-slices"])
                graph = q.records[4][1]
                self.assertEqual(graph[:2], ["selected-clang", "--target=native"])
                self.assertIn(str(ROOT / "tests/observation/test_source_graph.c"), graph)
                self.assertEqual(q.records[5][1], [str(q.artifacts / "source-graph-unit")])
                for family, argv in q.records[-5:]:
                    self.assertEqual(argv[argv.index("--compiler") + 1], str(q.compiler))
                    self.assertEqual(argv[argv.index("--opt") + 1], "0,1,2,3")
                self.assertEqual(q.records[-1][1][-2:], ["--cc", "selected-clang --target=native"])
                self.assertEqual(len(q.family_receipts), 5)
                self.assertEqual(q.timeouts, {"ordinary-imported-tests": 1800, "observed-source-imports": 1800,
                                              "observed-array-queries": 1800, "observed-primitive-slices": 3600, "observed-positional-slices": 3600})
                self.assertEqual(wrapper.expected_array_cases(levels), arrays.expected_cases(levels))
                self.assertEqual(wrapper.expected_slice_cases(levels), slices.expected_cases(levels))
                self.assertEqual(wrapper.expected_positional_cases(levels), positional.expected_cases(levels))
        def test_enrollment_selftest_timeout_routing(self):
            with tempfile.TemporaryDirectory() as temp:
                q = FakeQualification(Path(temp))
                with mock.patch.object(q, 'command', wraps=q.command) as command:
                    destination = wrapper.dispatch_enrollment_selftest(q)
                self.assertEqual(command.call_args.kwargs, {'timeout': 120})
                self.assertEqual(command.call_args.args[0], 'wrapper-enrollment-selftest')
                self.assertIn('--selftest-only', command.call_args.args[1])
                self.assertEqual(destination, q.artifacts / 'wrapper-enrollment-selftest')
        def test_real_timeout_cannot_publish_partial_pass(self):
            families = ["ordinary-imported-tests", "observed-source-imports", "observed-array-queries", "observed-primitive-slices", "observed-positional-slices"]
            real_command, real_receipt = wrapper.Qualification.command, wrapper.Qualification.receipt
            for index, family in enumerate(families):
                with self.subTest(family=family), tempfile.TemporaryDirectory() as temp:
                    q = FakeQualification(Path(temp)); original = q.command
                    q.failure = None; q.enrollment_selftest = None
                    (q.artifacts / "source-manifest.json").write_text(json.dumps(q.source_manifest))
                    def command(name, argv, **kwargs):
                        return real_command(q, name, argv, **kwargs) if name == family else original(name, argv, **kwargs)
                    q.command = command
                    q.run = lambda selected, cc: wrapper.dispatch_required_families(q, selected, cc)
                    q.receipt = lambda: real_receipt(q)
                    def timeout(argv, **kwargs):
                        raise subprocess.TimeoutExpired(argv, kwargs["timeout"], output=b"partial stdout", stderr=b"partial stderr")
                    argv = ["test_observation.py", "--compiler", str(q.compiler), "--artifacts", str(q.artifacts), "--opt", "0,1,2,3"]
                    with mock.patch.object(wrapper, "Qualification", return_value=q), \
                         mock.patch.object(wrapper.sys, "argv", argv), \
                         mock.patch.object(wrapper.subprocess, "run", side_effect=timeout) as launch, \
                         mock.patch("builtins.print") as printed:
                        with self.assertRaises(subprocess.TimeoutExpired): wrapper.main()
                    launch.assert_called_once()
                    self.assertEqual(launch.call_args.kwargs["timeout"], 3600 if family in ("observed-primitive-slices", "observed-positional-slices") else 1800)
                    printed.assert_not_called()
                    self.assertEqual([r["family"] for r in q.family_receipts], families[:index])
                    record = q.records[-1]
                    self.assertEqual(record["name"], family)
                    self.assertEqual(record["status"], "fail"); self.assertIs(record["timeout"], True)
                    self.assertIn("TimeoutExpired", record["error"])
                    destination = q.artifacts / family
                    self.assertEqual(json.loads((destination / "status.json").read_text()), record)
                    self.assertEqual((destination / "stdout").read_bytes(), b"partial stdout")
                    self.assertEqual((destination / "stderr").read_bytes(), b"partial stderr")
                    receipt = json.loads((q.artifacts / "result.json").read_text())
                    self.assertEqual(receipt["status"], "fail")
                    self.assertIn("TimeoutExpired", receipt["failure"])
                    self.assertEqual(len(receipt["required_families"]), index)
                    self.assertEqual(receipt["runs"][-1], record)
        def test_graph_build_failure(self):
            self._graph_failure("source-graph-unit-build")
        def test_graph_run_failure(self):
            self._graph_failure("source-graph-unit-run")
        def _graph_failure(self, failed):
            with tempfile.TemporaryDirectory() as temp:
                q = FakeQualification(Path(temp)); original = q.command
                def command(name, argv, **kwargs):
                    if name == failed: raise AssertionError("graph component failed")
                    return original(name, argv, **kwargs)
                q.command = command
                with self.assertRaises(AssertionError):
                    wrapper.pure_components(q, "cc")
                    wrapper.dispatch_required_families(q, levels, "cc")
                self.assertNotEqual(q.status, "pass")
                self.assertFalse(q.family_receipts)
        def test_literal_oracles(self):
            self.assertEqual([1 + (BASIC / name).read_bytes().count(b"\n") for name in BASIC_HASHES], [11, 17])
            self.assertEqual([sha(BASIC / name) for name in BASIC_HASHES], list(BASIC_HASHES.values()))
            self.assertEqual(len(BASIC_HITS), 7); self.assertEqual(len(BASIC_LINES), 8)
            self.assertTrue(7 * 100 >= 87 * 8); self.assertFalse(7 * 100 >= 88 * 8)
        def test_graph_known_answer(self):
            def file(fid, role, lookup, canonical, requested, declared, content, lines):
                return {"id": fid, "role": role, "lookup_path_bytes_hex": lookup.hex(),
                        "canonical_path_bytes_hex": canonical.hex(),
                        "requested_module_bytes_hex": requested.hex() if requested is not None else None,
                        "declared_module_bytes_hex": declared.hex(), "source_bytes": len(content),
                        "physical_line_count": lines, "source_sha256": hashlib.sha256(content).hexdigest()}
            model = {"invocation_directory_bytes_hex": b"/invoke".hex(),
                     "original_root_spelling_bytes_hex": b"entry/root.sts".hex(),
                     "files": [file(0, 0, b"/invoke/entry/root.sts", b"/target/root.sts", None, b"root", (BASIC / "root.sts").read_bytes(), 11),
                               file(1, 1, b"/invoke/entry/raw\xff.sts", b"/target/raw\xff.sts", b"raw", b"raw", b"mod raw;\n", 2)],
                     "edges": [{"parent_file_id": 0, "child_file_id": 1, "line": 2, "col": 1, "ordinal": 0,
                                "requested_module_bytes_hex": b"raw".hex(), "lookup_path_bytes_hex": b"/invoke/entry/raw\xff.sts".hex()}]}
            self.assertEqual(recompute_graph(model), "6134956c38d24a31674d40d1d65308619b177cb3168a23bfa059d097267d126a")
        def test_map_known_answer(self):
            self.assertEqual(recompute_map(map_known_answer(), version=3), "ee3bd2ad5c88b2fb90e257277618654d5e7498d4bf221ca06cfcebe8b6689a60")
            self.assertEqual(recompute_map(map_known_answer()), "3813ba52b6d33367990b5cbd004a984ec1c6f53e22e21a8b6d5f259d0957f12c")
        def test_config_known_answers(self):
            digest, value = config_digest("dd" * 32, "aa" * 32, 2, _version=3)
            self.assertEqual(len(value), 526)
            self.assertEqual(digest, "1dc9f3d3e2be65e986cf997e1350dea939ad3517d392f67a3d9220f98d3ed457")
            digest, value = config_digest("dd" * 32, "aa" * 32, 2, enabled=1, maximum=None, minimum=None, _version=3)
            self.assertEqual(len(value), 514)
            self.assertEqual(digest, "73e35427155369e8f0650b12ee31635baa2f8494cbfca23e3959ce73fa0dd953")
            digest, value = config_digest("dd" * 32, "aa" * 32, 2)
            self.assertEqual(len(value), 559)
            self.assertEqual(digest, "32266849eed635082fdc9753111d97ff4bbeaaf9d95f07cf66c5d31fc90b1654")
        def test_strict_json_duplicates(self):
            for text in ('{"a":1,"a":2}', '{"x":NaN}', '{"x":Infinity}'):
                self.assertRaises((ValueError, AssertionError), strict_json, text)
        def test_complete_raw_v4_known_answer(self):
            model, report = raw_report_known_answer()
            self.assertEqual(validate_raw_report(model, report)[-1], 'END')
            section = report[report.index('allocations '):]
            self.assertEqual(len(section), 86)
            self.assertEqual(hashlib.sha256(section.encode()).hexdigest(), '14074eb03ad114dead37a8c2b9edfd4676485af698b86f605e6c303ce6ebf30c')
        def test_complete_raw_rejects_every_unconsumed_byte(self):
            model, report = raw_report_known_answer()
            mutations = (report[:-1], report+'\n', report+'END\n', report.replace('enabled 3', 'enabled 03'),
                report.replace('enabled 3', 'enabled +3'), report.replace('enabled 3', 'enabled  3'),
                report.replace('runtime_abi 4', 'runtime_abi 3'), report.replace('STASHA_OBSERVATION 4', 'STASHA_OBSERVATION 3'),
                report.replace('metric_versions 2 2', 'metric_versions 1 2'), report.replace('state 1 0 1', 'state 0 1 0'),
                report.replace('test 0 2', 'test 0 2\ntest 0 2'), report.replace('allocations 2 27', 'allocations 2 28'),
                report.replace('allocation 1 1 7 0 4294967295 0\n', ''), report.replace('hits 01', 'hits ff'),
                report.replace('hits 01', 'hits 0100'), report.replace('allocations 2 27\n',''), report.replace('tests 1 1','tests 1 0'))
            for bad in mutations:
                with self.subTest(report=bad): self.assertRaises(AssertionError, validate_raw_report, model, bad)
        def test_raw_domain_metadata_and_totals_fail_closed(self):
            model, report = raw_report_known_answer()
            def clone(): return json.loads(json.dumps(model))
            cases=[]
            for key,value in [('domain',0),('domain',3),('bytes',0),('bytes',21),('bytes',4*2147483648),
                              ('generation',1),('generation',2**64),('test',1),('allocation_site',99),('allocation_site',0),('resize_site',1)]:
                m=clone(); m['allocations'][1][key]=value; cases.append(m)
            for key,value in [('slice_element_kind',0),('slice_element_kind',12),('slice_abi_stride',8),('slice_count_arity',0),('slice_count_arity',3),('kind',3)]:
                m=clone();m['sites'][3][key]=value;cases.append(m)
            m=clone();m['sites'][0]['slice_count_arity']=1;cases.append(m)
            m=clone();m['allocations'][0]['domain']=2;cases.append(m)
            m=clone();m['allocation_domains']['generated_primitive_slice_backing']['tracked_outstanding_bytes']=0;cases.append(m)
            m=clone();m['allocation_domains']['unmeasured_provider']={'live_allocations':0,'tracked_outstanding_bytes':0};cases.append(m)
            m=clone();m['tracked_outstanding_bytes']=28;cases.append(m)
            m=clone();m['complete']=0;cases.append(m)
            m=clone();m['tests_completed']=0;cases.append(m)
            m=clone();m['allocations']=list(reversed(m['allocations']));cases.append(m)
            for bad in cases:
                with self.subTest(model=bad): self.assertRaises((AssertionError,KeyError), validate_raw_report, bad, report)
        def test_maximum_strides_are_pure_metadata_only(self):
            # Never materialize or execute a huge native source: integer records only.
            model, report = raw_report_known_answer()
            for element,stride in enumerate((1,1,2,2,4,4,8,8,1,4,8),1):
                for capacity in (0,2147483647,2147483648):
                    m=json.loads(json.dumps(model)); size=capacity*stride
                    m['sites'][3].update(slice_element_kind=element,slice_abi_stride=stride)
                    m['allocations'][1]['bytes']=size
                    m['tracked_outstanding_bytes']=size+7
                    m['allocation_domains']['generated_primitive_slice_backing']['tracked_outstanding_bytes']=size
                    raw=report.replace('allocations 2 27',f'allocations 2 {size+7}').replace('allocation 2 2 20 3',f'allocation 2 2 {size} 3')
                    if capacity==2147483647: validate_raw_report(m,raw)
                    else: self.assertRaises(AssertionError,validate_raw_report,m,raw)
        def test_positive_nonlaunch_expected_link_failure(self):
            with tempfile.TemporaryDirectory() as temp:
                result = subprocess.CompletedProcess([], 1, b"", b"hosted Linux linking requires a working native C compiler; set CC to its executable path\nlinking failed\n")
                validate_positive_nonlaunch_result(result, Path(temp) / "absent-image")
        def test_positive_nonlaunch_wrong_failure_rejected(self):
            with tempfile.TemporaryDirectory() as temp:
                for stderr in [b"linking failed", b"hosted Linux linking requires a working native C compiler", b"other error"]:
                    with self.subTest(stderr=stderr):
                        result = subprocess.CompletedProcess([], 1, b"", stderr)
                        self.assertRaises(AssertionError, validate_positive_nonlaunch_result, result, Path(temp) / "absent-image")
        def test_positive_nonlaunch_later_compiler_failures_rejected(self):
            with tempfile.TemporaryDirectory() as temp:
                expected = b"hosted Linux linking requires a working native C compiler\nlinking failed\n"
                for diagnostic in [b"LLVM IR verification failed", b"LLVM IR verification failed after optimization", b"code generation failed", b"optimization failed", b"failed to run optimization passes", b"internal compiler error", b"LLVM ERROR: invalid module", b"Assertion failed", b"invalid debug metadata"]:
                    with self.subTest(diagnostic=diagnostic):
                        result = subprocess.CompletedProcess([], 1, b"", expected + diagnostic)
                        self.assertRaises(AssertionError, validate_positive_nonlaunch_result, result, Path(temp) / "absent-image")
        def test_positive_nonlaunch_stdout_failure_rejected(self):
            with tempfile.TemporaryDirectory() as temp:
                result = subprocess.CompletedProcess([], 1, b"LLVM IR verification failed", b"hosted Linux linking requires a working native C compiler\nlinking failed\n")
                self.assertRaises(AssertionError, validate_positive_nonlaunch_result, result, Path(temp) / "absent-image")
        def test_positive_nonlaunch_success_status_rejected(self):
            with tempfile.TemporaryDirectory() as temp:
                result = subprocess.CompletedProcess([], 0, b"", b"hosted Linux linking requires a working native C compiler\nlinking failed\n")
                self.assertRaises(AssertionError, validate_positive_nonlaunch_result, result, Path(temp) / "absent-image")
        def test_positive_nonlaunch_image_rejected(self):
            with tempfile.TemporaryDirectory() as temp:
                image = Path(temp) / "unexpected-image"; image.write_bytes(b"mock image, never executed")
                result = subprocess.CompletedProcess([], 1, b"", b"hosted Linux linking requires a working native C compiler\nlinking failed\n")
                self.assertRaises(AssertionError, validate_positive_nonlaunch_result, result, image)
        def test_leaf_import_exact_snapshot_diagnostic(self):
            validate_leaf_rejection_diagnostic("import", b"cannot snapshot bounded regular source '/tmp/deliberately_missing.sts' (missing)")
        def test_leaf_import_other_module_rejected(self):
            self.assertRaises(AssertionError, validate_leaf_rejection_diagnostic, "import", b"cannot snapshot bounded regular source '/tmp/other.sts' (missing)")
        def test_leaf_import_old_generic_diagnostic_rejected(self):
            self.assertRaises(AssertionError, validate_leaf_rejection_diagnostic, "import", b"unsupported by observation profile: deliberately_missing.sts")
        def test_other_leaf_diagnostic_unchanged(self):
            validate_leaf_rejection_diagnostic("macro", b"unsupported by observation profile: macros")
            self.assertRaises(AssertionError, validate_leaf_rejection_diagnostic, "macro", b"cannot snapshot bounded regular source '/tmp/deliberately_missing.sts'")
        def test_rejection_stage_partition(self):
            self.assertEqual(set(REJECTIONS), set(BENIGN_REJECTIONS) | NO_LAUNCH_NAMES)
            self.assertFalse(set(BENIGN_REJECTIONS) & NO_LAUNCH_NAMES)
            self.assertNotIn("compile_only", expected_cases(levels))
        def test_all_array_dependencies_mandatory(self):
            for missing_name in ["tests/observation/test_primitive_slices.py", "tests/observation/test_array_queries.py", "tests/observation/test_positional_slices.py", "tests/memory/run_positional_make.py", "tests/memory/test_positional_make.py", "tests/observation/fixtures/positional_slices/corpus.json", *arrays.DEPENDENCY_HASHES]:
                def missing(path):
                    if path == ROOT / missing_name: raise FileNotFoundError(path)
                    return b"mock frozen input"
                with self.subTest(path=missing_name), mock.patch.object(Path, "read_bytes", missing):
                    self.assertRaises(FileNotFoundError, input_manifest)
        def test_unknown_family_identity_rejected(self):
            with tempfile.TemporaryDirectory() as temp:
                self.assertRaises(AssertionError, FamilySession, "unknown", None, Path(temp), levels, runtime=False)
        def test_observed_runtime_required(self):
            for family in ("observed-source-imports", "observed-array-queries", "observed-primitive-slices", "observed-positional-slices"):
                with tempfile.TemporaryDirectory() as temp:
                    self.assertRaises(AssertionError, FamilySession, family, None, Path(temp), levels, runtime=False)
        def test_array_pure_components(self):
            with tempfile.TemporaryDirectory() as temp:
                self.assertEqual(arrays.selftest(Path(temp)), 0)
        def test_slice_pure_components(self):
            with tempfile.TemporaryDirectory() as temp:
                self.assertEqual(slices.selftest(Path(temp)), 0)
        def test_positional_pure_components(self):
            with tempfile.TemporaryDirectory() as temp:
                self.assertEqual(positional.selftest(Path(temp)), 0)
        def test_positional_ordinary_pure_components(self):
            module = load_module('enrollment_positional_ordinary_tests', ROOT / 'tests/memory/test_positional_make.py')
            result = unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromModule(module))
            self.assertTrue(result.wasSuccessful())
        def test_aggregate_subset_fails_before_dispatch(self):
            with tempfile.TemporaryDirectory() as temp:
                q = FakeQualification(Path(temp))
                self.assertRaises(AssertionError, wrapper.dispatch_required_families, q, [0, 2, 3], 'cc')
                self.assertEqual(q.records, [])
        def test_missing_mandatory_runner_fails(self):
            original = Path.read_bytes
            def missing(path):
                if path == ROOT / "tests/scalars/test_imported_tests.py": raise FileNotFoundError(path)
                return b"mock frozen input"
            with mock.patch.object(Path, "read_bytes", missing):
                self.assertRaises(FileNotFoundError, input_manifest)
        def test_changed_frozen_source_fails(self):
            with tempfile.TemporaryDirectory() as temp:
                q = FakeQualification(Path(temp), "changed-parent-source")
                self.assertRaises(AssertionError, wrapper.dispatch_required_families, q, levels, "cc")
                self.assertNotEqual(q.status, "pass")

    def failure_test(mutation, family):
        def test(self):
            with tempfile.TemporaryDirectory() as temp:
                q = FakeQualification(Path(temp), mutation, family)
                with self.assertRaises((AssertionError, OSError, ValueError, KeyError, subprocess.TimeoutExpired)):
                    wrapper.dispatch_required_families(q, levels, "cc")
                self.assertNotEqual(q.status, "pass")
        return test
    mutations = ["nonzero", "timeout", "exception", "missing-latest", "missing-receipt", "malformed", "running", "fail", "skipped",
                 "compiler", "runner", "optimizations", "source", "missing-case", "duplicate-case", "skipped-case", "count", "run-status", "zero-executions", "outside-receipt", "missing-generated", "changed-generated", "family", "unexpected-case", "duplicate-run", "artifact-hash", "artifact-path", "missing-artifact"]
    for family in expected_by_family:
        for mutation in mutations + (["runtime", "profile", "duplicate-run-id"] if FAMILY_IDENTITIES[family][1] else []) + (["positional-contract", "positional-barrier", "positional-missing-command", "positional-reordered-command"] if family == "observed-positional-slices" else []):
            setattr(EnrollmentTests, "test_" + family.replace("-", "_") + "_" + mutation.replace("-", "_"), failure_test(mutation, family))
    with mock.patch("subprocess.run", side_effect=AssertionError("selftests may not spawn processes")):
        result = unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromTestCase(EnrollmentTests))
    artifacts.mkdir(parents=True, exist_ok=True)
    (artifacts / "result.json").write_text(json.dumps({"schema": 2, "status": "pass" if result.wasSuccessful() else "fail",
        "family": "wrapper-enrollment-selftest", "tests_run": result.testsRun,
        "failures": len(result.failures), "errors": len(result.errors), "native_executions": 0}, indent=2) + "\n")
    return 0 if result.wasSuccessful() else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path)
    parser.add_argument("--ir-compiler", type=Path, help="separately reviewed exact-object fork-disabled compiler; local gate only")
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--opt", default="0,1,2,3")
    parser.add_argument("--cc", default="cc", help="selected host compiler identity retained by wrapper; native images use Stasha")
    parser.add_argument("--stage", choices=("all", "prepare", "compile-only", "rejections", "native"), default="all")
    parser.add_argument("--selftest-only", action="store_true")
    args = parser.parse_args()
    if args.selftest_only: return enrollment_selftest(args.artifacts)
    levels = parse_levels(args.opt, parser)
    if args.stage != "prepare" and args.compiler is None: parser.error("--compiler required for qualification")
    if args.stage == "compile-only" and args.ir_compiler is None: parser.error("--compile-only requires --ir-compiler reviewed nonlaunch image")
    session = FamilySession("observed-source-imports", args.compiler, args.artifacts, levels, runtime=True)
    try:
        positive = {name: write_sources(session.artifacts / "sources/positive" / name, files) for name, files in positive_sources().items()}
        positive["symlink-requested-directory"] = session.artifacts / "sources/positive/symlink-requested-directory/entry/root.sts"
        negative = {name: write_sources(session.artifacts / "sources/rejections" / name, files) for name, files in REJECTIONS.items()}
        leaf = load_module("leaf_stage_sources", ROOT / "tests/observation/test_observation.py")
        for name, body in leaf.REJECTIONS.items():
            write_sources(session.artifacts / "sources/leaf-rejections", {name + ".sts": "mod reject;\n" + body + "\ntest 'must not run' { print.('UNSUPPORTED_RAN\\n'); }\n"})
        for name, control in leaf.COMPILE_ONLY_CONTROLS.items():
            write_sources(session.artifacts / "sources/leaf-controls", {name + ".sts": control["source"]})
        ordinary = load_module("ordinary_stage_sources", ROOT / "tests/scalars/test_imported_tests.py")
        ordinary_sources = {name: ordinary.materialize(session.artifacts / "sources/ordinary-ir", name)[0] for name in ordinary.CASES}
        session.freeze_generated()
        inventory = compile_only_inventory(levels) if args.stage == "compile-only" else expected_cases(levels)
        session.write_inventory(inventory)
        # Prepared inventory is explicitly nonterminal and cannot satisfy parent success.
        if args.stage == "prepare": session.status = "prepared"
        elif args.stage == "compile-only":
            run_compile_only(session, positive, negative, ordinary_sources, levels, args.ir_compiler)
            session.finish(inventory)
        else:
            inventory = expected_cases(levels, args.stage); session.write_inventory(inventory)
            if args.stage in ("all", "rejections"): run_rejections(session, negative)
            if args.stage in ("all", "native"): run_native(session, positive, levels)
            session.finish(inventory)
    except BaseException as error:
        session.status, session.failure = "fail", repr(error)
        raise
    finally:
        session.receipt()
    print(f"{session.status.upper()}: direct-source {args.stage}; evidence {session.artifacts}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
