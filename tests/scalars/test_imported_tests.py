#!/usr/bin/env python3
"""Ordinary imported test-context regressions; compilation and execution are separate.

No observation flags or observer linkage are permitted. Duplicate import behavior
is deliberately outside this family. --prepare writes the fixed source inventory
without invoking a compiler or a generated image.
"""
from __future__ import annotations
import argparse
import importlib.util
import json
from pathlib import Path
import re
import sys

if not __debug__:
    raise RuntimeError("qualification requires Python assertions enabled")
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("source_import_support", ROOT / "tests/observation/test_source_imports.py")
_support = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_support)
FamilySession = _support.FamilySession
parse_levels = _support.parse_levels

# Every marker and assertion count is an independent literal oracle. Identical
# labels are intentional; labels cannot supply test identity or execution order.
CASES = {
    "module-context": ({
        "root.sts": "mod root;\nimp dep;\nstack i32 state = 100;\nfn same(stack i32 x): i32 { ret x + state; }\ntest 'same label' { print.('CTX_ROOT_0\\n'); expect_eq.(same(1), 101); expect_eq.(state, 100); }\ntest 'same label' { print.('CTX_ROOT_1\\n'); expect_eq.(same(2), 102); }\n",
        "dep.sts": "mod dep;\nstack i32 state = 7;\nfn same(stack i32 x): i32 { ret x + state; }\ntest 'same label' { print.('CTX_DEP_0\\n'); expect_eq.(same(1), 8); expect_eq.(state, 7); }\ntest 'same label' { print.('CTX_DEP_1\\n'); expect_eq.(same(2), 9); }\n",
    }, ["CTX_ROOT_0", "CTX_ROOT_1", "CTX_DEP_0", "CTX_DEP_1"], 6, 4),
    "root-after-leaf-helper": ({
        "root.sts": "mod root;\nimp dep;\nstack i32 value = 19;\nfn helper(void): i32 { ret value; }\ntest 'root context' { print.('ROOT_AFTER_HELPER\\n'); expect_eq.(helper(), 19); }\n",
        "dep.sts": "mod dep;\nstack i32 value = 71;\nfn helper(void): i32 { ret value; }\nfn last_function(void): bool { ret true; }\n",
    }, ["ROOT_AFTER_HELPER"], 1, 1),
    "imported-no-helper": ({
        "root.sts": "mod root;\nimp dep;\nstack i32 value = 91;\nfn last_function(void): i32 { ret value; }\ntest 'root' { print.('NO_HELPER_ROOT\\n'); expect_eq.(value, 91); }\n",
        "dep.sts": "mod dep;\nstack i32 value = 23;\ntest 'imported' { print.('NO_HELPER_DEP\\n'); expect_eq.(value, 23); }\n",
    }, ["NO_HELPER_ROOT", "NO_HELPER_DEP"], 2, 2),
    "empty-imported-test": ({
        "root.sts": "mod root;\nimp dep;\nfn last_function(void): i32 { ret 9; }\ntest 'root' { print.('EMPTY_ROOT\\n'); expect_eq.(last_function(), 9); }\n",
        "dep.sts": "mod dep;\ntest 'empty imported' { }\ntest 'after empty' { print.('EMPTY_DEP\\n'); expect.(true); }\n",
    }, ["EMPTY_ROOT", "EMPTY_DEP"], 2, 3),
    "global-sibling-alias": ({
        "root.sts": "mod root;\nimp left;\nimp right;\ntest 'same label' { print.('SIBLING_ROOT\\n'); expect_eq.(left.work(), 11); }\n",
        "left.sts": "mod left;\next fn work(void): i32 { ret right.work() + 1; }\ntest 'same label' { print.('SIBLING_LEFT\\n'); expect_eq.(work(), 11); }\n",
        "right.sts": "mod right;\next fn work(void): i32 { ret 10; }\ntest 'same label' { print.('SIBLING_RIGHT\\n'); expect_eq.(work(), 10); }\n",
    }, ["SIBLING_ROOT", "SIBLING_LEFT", "SIBLING_RIGHT"], 3, 3),
    "symlink-requested-directory": ({
        "target/root.sts": "mod root;\nimp dep;\ntest 'requested directory' { print.('SYMLINK_ROOT\\n'); expect_eq.(dep.value(), 7); }\n",
        "target/dep.sts": "mod dep;\next fn value(void): i32 { ret 99; }\n",
        "entry/dep.sts": "mod dep;\next fn value(void): i32 { ret 7; }\n",
    }, ["SYMLINK_ROOT"], 1, 1),
}

CASES["fresh-local-provenance"] = ({
    "root.sts": "mod root;\nimp dep;\nfn prior(void): void { heap u8 *rw p = new.(1); rem.(p); }\ntest 'first reused local' { print.('FRESH_ROOT_0\\n'); heap u8 *rw p = new.(3); expect.(p != nil); rem.(p); }\ntest 'second reused local' { print.('FRESH_ROOT_1\\n'); heap u8 *rw p = new.(5); expect.(p != nil); rem.(p); }\n",
    "dep.sts": "mod dep;\ntest 'imported reused local' { print.('FRESH_DEP\\n'); heap u8 *rw p = new.(7); expect.(p != nil); rem.(p); }\n",
}, ["FRESH_ROOT_0", "FRESH_ROOT_1", "FRESH_DEP"], 3, 3)

# Reversing imports changes test traversal order, not lexical binding.
_sources, _, _, _ = CASES["global-sibling-alias"]
CASES["reversed-import-order"] = ({**_sources, "root.sts": _sources["root.sts"].replace("imp left;\nimp right;", "imp right;\nimp left;")},
    ["SIBLING_ROOT", "SIBLING_RIGHT", "SIBLING_LEFT"], 3, 3)


def expected_cases(levels):
    return [f"{name}-O{opt}" for opt in levels for name in CASES]


def materialize(work, name):
    sources, markers, assertions, tests = CASES[name]
    folder = work / name
    for filename, source in sources.items():
        path = folder / filename
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source, encoding="utf-8", newline="\n")
    if name == "symlink-requested-directory":
        entry = folder / "entry/root.sts"
        entry.symlink_to("../target/root.sts")
    else:
        entry = folder / "root.sts"
    return entry, markers, assertions, tests


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--opt", default="0,1,2,3")
    parser.add_argument("--prepare", action="store_true")
    args = parser.parse_args()
    levels = parse_levels(args.opt, parser)
    if not args.prepare and args.compiler is None:
        parser.error("--compiler is required unless --prepare is selected")
    session = FamilySession("ordinary-imported-tests", args.compiler, args.artifacts, levels, runtime=False)
    try:
        fixtures = {name: materialize(session.artifacts / "sources", name) for name in CASES}
        session.freeze_generated()
        session.write_inventory({"ordinary_native": expected_cases(levels)})
        if args.prepare:
            session.status = "prepared"
        else:
            for opt in levels:
                for name in CASES:
                    entry, markers, assertions, tests = fixtures[name]
                    key = f"{name}-O{opt}"
                    binary = session.artifacts / (key + ".test")
                    _, compiled = session.command(key + "-compile", [str(session.compiler), "test", str(entry), f"-o={opt}", "-o", str(binary)])
                    assert binary.is_file(), key + ": missing fresh binary"
                    assert all(marker.encode() not in compiled.stdout for marker in markers), key + ": compilation ran tests"
                    assert b"__stasha_obs_" not in binary.read_bytes(), key + ": observer linkage in ordinary image"
                    image_hash = _support.sha(binary)
                    _, executed = session.command(key + "-execute", [str(binary)])
                    assert _support.sha(binary) == image_hash, key + ": executed image changed"
                    assert executed.stderr == b"", (key, executed.stderr)
                    output = executed.stdout.decode("utf-8")
                    positions = []
                    for marker in markers:
                        assert output.count(marker + "\n") == 1, (key, marker, output)
                        positions.append(output.index(marker + "\n"))
                    assert positions == sorted(positions), key + ": test ordering changed"
                    assert output.count("PASS\n") == tests and "FAIL\n" not in output, (key, output)
                    assert re.search(rf"\n{assertions} passed, 0 failed\n\Z", output), (key, output)
                    session.cases.append({"name": key, "stage": "ordinary_native", "status": "pass", "compile_only": False,
                                          "executions": 1, "assertions_passed": assertions, "tests": tests,
                                          "binary_sha256": image_hash})
            session.finish({"ordinary_native": expected_cases(levels)})
    except BaseException as error:
        session.status, session.failure = "fail", repr(error)
        raise
    finally:
        session.receipt()
    print(f"{session.status.upper()}: ordinary imported tests; evidence {session.artifacts}")


if __name__ == "__main__":
    main()
