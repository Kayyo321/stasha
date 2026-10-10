#!/usr/bin/env python3
"""Fail-closed diagnostic and C-interop test oracles (Python 3, no packages)."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
ERROR = r"^error(?:\[[^\]]+\])?:"
WARNING = r"^warning(?:\[[^\]]+\])?:"
# A crash handler may convert a signal to exit 1. Such output is never a
# successful negative test, even if an expected diagnostic preceded it.
INTERNAL_FAILURE = re.compile(
    r"LLVM IR verification failed|LLVM ERROR:|Broken (?:function|module) found|"
    r"internal compiler error|Assertion .* failed|Assertion failed|"
    r"AddressSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|"
    r"runtime error:|Segmentation fault|Bus error|stack dump:|"
    r"fatal signal|compiler crash",
    re.IGNORECASE,
)


def diag_cases():
    """Keep legacy assertions explicit; semantic drift must stay visible/red."""
    def case(name, pattern, *, outcome="rejection", flags=(), count=None, absent=()):
        return dict(name=name + ("/" + "/".join(flags) if flags else ""),
                    source="tests/diag/" + name + ".sts", outcome=outcome,
                    flags=list(flags), patterns=[pattern], absent=list(absent), count=count)

    return [
        case("dedup_undefined_var", ERROR + r".*undefined variable", count=1),
        case("dedup_undefined_fn", ERROR + r".*undefined function", count=1),
        dict(case("dedup_undefined_type", ERROR + r".*unknown type", count=1),
             patterns=[ERROR + r".*unknown type", "did you mean"]),
        dict(case("dedup_undefined_member", ERROR + r".*cannot resolve member", count=1),
             patterns=[ERROR + r".*cannot resolve member", "did you mean"]),
        case("early_abort_type_error", ERROR, absent=["undefined variable 'x'"]),
        case("parser_recovery_multi_error", ERROR),
        case("category_undefined", r"^error\[undefined\]:"),
        case("category_storage_domain", r"^error\[storage\]:"),
        case("levenshtein_variable", "did you mean"),
        case("levenshtein_type", "did you mean"),
        case("warn_unreachable_code", WARNING + ".*unreachable", outcome="success", flags=["--wall"]),
        case("warn_shadow_variable", WARNING + ".*shadow", outcome="success", flags=["--wall"]),
        case("warn_unused_with_wall", WARNING + ".*unused", outcome="success", flags=["--wall"]),
        case("warn_unused_with_wall", WARNING + ".*unused", outcome="success"),
        case("strict_promotes_warning", ERROR, flags=["--strict"], absent=[WARNING + ".*unused"]),
        case("strict_promotes_warning", WARNING, outcome="success"),
        # This historical expectation conflicts with newer await documentation.
        # It is deliberately not weakened or silently marked xfail here.
        case("coro_await_outside_async", "only legal inside 'async fn'"),
        case("coro_yield_outside_async", "'yield' is only legal inside 'async fn'"),
        case("coro_mixed_yield_types", "must share one type"),
        case("coro_stream_nonvoid_ret", "cannot return a final value"),
        case("coro_async_dispatch_stream", "cannot dispatch yielding async function"),
        case("coro_await_next_requires_stream", "expects a stream"),
        dict(case("dedup_undefined_var", r"^error: aborting due to"), name="dedup_undefined_var/summary"),
    ]


def interop_cases():
    names = [
        "01_scalars", "02_ptrs", "03_const_ptr", "04_struct_byval", "05_struct_ptr",
        "06_nested", "07_array_field", "08_anon_typedef", "09_opaque", "10_enum",
        "11_define", "12_varargs", "13_callback_to_c", "14_va_access", "15_va_foreach",
        "16_va_typed", "18_bool", "19_qualifiers",
        "20_globals", "21_outbuf", "22_outarray", "24_nested_include", "25_search_path",
        "26_system_header", "30_errno", "51_b_scalars", "52_b_struct_byval",
        "53_b_callback", "54_b_ctor", "55_b_multi_tu",
    ]
    cases = [dict(name=n, source="tests/cinterop/" + n + ".sts", outcome="success",
                  stdout=n + ": ok", support=n not in ["14_va_access", "15_va_foreach", "16_va_typed"]) for n in names]
    cases.append(dict(name="llvm_smoke", source="tests/cinterop/llvm_smoke.sts",
                      outcome="success", stdout="ModuleID", llvm=True))
    for name, pattern in [("bitfield_unsupported", "bitfield"),
                          ("long_double_unsupported", "long double"),
                          ("union_layout", "union")]:
        cases.append(dict(name="neg/" + name, source="tests/cinterop/neg/" + name + ".sts",
                          outcome="rejection", patterns=[ERROR + ".*" + pattern]))
    cases.append(dict(name="xfail/04b_struct_byval_arg",
                      source="tests/cinterop/xfail/04b_struct_byval_arg.sts",
                      outcome="success", stdout="04b_struct_byval_arg: ok", support=True,
                      xfail=dict(system="Darwin", machines=["arm64", "aarch64"],
                                 exit=1, stdout="XFAIL struct-byval arg got wrong value",
                                 reason="Known AAPCS64 aggregate-argument ABI lowering defect")))
    return cases


def signal_cases():
    # Preserve the original byte/order assertions (shell command substitution
    # ignored trailing newlines). quit_from_exit deliberately exits 1.
    expectations = [
        ("basic", 0, "basic: code=42"),
        ("multi_watcher", 0, "A:7\nB:7"),
        ("break", 0, "X:1\nY:1\nY:2"),
        ("recursive", 0, "d=1 c=5\nd=2 c=0\nNEW:0"),
        ("nested_loop_break", 0, "enter:1\ni=0\nafter\nenter:2\ni=0\nafter"),
        ("quit_runs_exit", 0, "before_quit\nexit_block_ran"),
        ("quit_from_exit", 1, "exit_block_ran"),
    ]
    return [dict(name=name, source="tests/signals/" + name + ".sts", outcome="success",
                 stdout=stdout, stdout_exact=True, runtime_exit=code)
            for name, code, stdout in expectations]


def category(code, timed_out=False):
    if timed_out:
        return "timeout"
    if code == 0:
        return "success"
    if code == 1:
        return "rejection"
    # Signals are negative with direct POSIX subprocesses; shell wrappers use
    # 128+signal. Windows exceptions use high unsigned or signed status codes.
    if code < 0 or code >= 128:
        return "crash"
    return "unexpected-exit"


def capture(command, cwd, prefix, timeout, extra_env=None):
    """Capture streams separately, bound execution, retain exact provenance."""
    start = time.monotonic()
    result = {"command": [str(x) for x in command], "cwd": str(cwd), "timeout_seconds": timeout}
    stdout_path, stderr_path = Path(str(prefix) + ".stdout"), Path(str(prefix) + ".stderr")
    env = dict(os.environ, NO_COLOR="1", TERM="dumb")
    env.update(extra_env or {})
    result["environment_overrides"] = dict(NO_COLOR="1", TERM="dumb", **(extra_env or {}))
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        try:
            process = subprocess.Popen(result["command"], cwd=cwd, env=env, stdout=stdout,
                                       stderr=stderr, start_new_session=(os.name == "posix"))
            timed_out = False
            try:
                code = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                if os.name == "posix":
                    # Kill the entire session, including spawned linker/test children.
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                else:
                    # taskkill is part of Windows; /T also terminates child processes.
                    try:
                        subprocess.run(["taskkill", "/F", "/T", "/PID", str(process.pid)],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                       check=False, timeout=5)
                    except (OSError, subprocess.TimeoutExpired):
                        pass
                    try:
                        process.kill()
                    except ProcessLookupError:
                        pass
                code = process.wait()
            result.update(returncode=code, category=category(code, timed_out))
        except OSError as error:
            result.update(returncode=None, category="launch-error", error=str(error))
            stderr.write((str(error) + "\n").encode())
    result["elapsed_seconds"] = round(time.monotonic() - start, 6)
    result["stdout_file"], result["stderr_file"] = str(stdout_path), str(stderr_path)
    result["stdout"] = ANSI.sub("", stdout_path.read_text(errors="replace"))
    result["stderr"] = ANSI.sub("", stderr_path.read_text(errors="replace"))
    Path(str(prefix) + ".json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def compiler_problems(result, case):
    problems = []
    if result["category"] != case["outcome"]:
        problems.append("compiler %s (exit=%s); expected %s" %
                        (result["category"], result["returncode"], case["outcome"]))
    text = result["stderr"]
    if INTERNAL_FAILURE.search(text + "\n" + result["stdout"]):
        problems.append("compiler internal/verifier/sanitizer failure")
    if case["outcome"] == "success" and re.search(ERROR + r"|^\(EE\)", text, re.MULTILINE):
        problems.append("compiler emitted an error despite success status")
    if case["outcome"] == "rejection":
        if not re.search(ERROR, text, re.MULTILINE):
            problems.append("missing error diagnostic on stderr")
        # Require an actual source location, not just the fixture's name in a
        # command echo. Exact columns can vary with diagnostic improvements.
        source_name = re.escape(Path(case["source"]).name)
        if not re.search(source_name + r":[1-9][0-9]*(?::[1-9][0-9]*)?(?:\s|$)", text):
            problems.append("missing source location for " + Path(case["source"]).name)
    for index, pattern in enumerate(case.get("patterns", [])):
        matches = re.findall(pattern, text, re.MULTILINE)
        expected_count = case.get("count") if index == 0 else None
        if expected_count is not None and len(matches) != expected_count:
            problems.append("diagnostic count: expected %d, got %d for %r" %
                            (expected_count, len(matches), pattern))
        elif not matches:
            problems.append("missing diagnostic pattern " + repr(pattern))
    for pattern in case.get("absent", []):
        if re.search(pattern, text, re.MULTILINE):
            problems.append("unexpected diagnostic pattern " + repr(pattern))
    return problems


def xfail_applies(case):
    rule = case.get("xfail")
    return bool(rule and platform.system() == rule["system"] and
                platform.machine().lower() in rule["machines"])


def run_case(case, args, artifact_dir):
    work = artifact_dir / re.sub(r"[^a-zA-Z0-9_.-]", "_", case["name"])
    work.mkdir()
    result = {"name": case["name"], "expected": case, "status": "FAIL", "problems": []}
    source = ROOT / case["source"]
    if not source.is_file():
        result["problems"] = ["missing required fixture: " + str(source)]
        return result
    link_args = []
    if case.get("support"):
        if not args.support_lib.is_file():
            result["problems"] = ["missing support archive; run make tests/cinterop/build/libcinterop_support.a"]
            return result
        link_args += ["-l", str(args.support_lib)]
    if case.get("llvm"):
        if not args.llvm_config.is_file():
            reason = "in-tree llvm-config unavailable; run make llvm"
            result.update(status="FAIL" if args.require_prerequisites else "SKIP", problems=[reason])
            return result
        config = capture([str(args.llvm_config), "--libfiles", "core", "analysis", "native", "lto",
                          "passes", "option", "codegen", "bitwriter", "debuginfodwarf", "objcarcopts",
                          "textapi", "object"], ROOT, work / "llvm-config", args.compile_timeout)
        result["llvm_config"] = config
        if config["category"] != "success" or not config["stdout"].strip():
            result["problems"] = ["llvm-config failed or returned no archives"]
            return result
        # llvm-config emits a shell-quoted library list on supported POSIX hosts.
        for library in shlex.split(config["stdout"]):
            link_args += ["-l", library]
    # This directory is new for every invocation AND case. Never consult the
    # checked-in tests/cinterop/build executables or reuse an earlier output.
    output = work / ("program.exe" if os.name == "nt" else "program")
    result["source_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
    compiled = capture([args.compiler, "build", str(source), "-o", str(output)] +
                       link_args + case.get("flags", []), ROOT, work / "compile", args.compile_timeout,
                       extra_env={"STS_DUMP_IR_PRE": str(work / "pre.ll"),
                                  "STS_DUMP_IR": str(work / "post.ll")})
    result["compile"] = compiled
    result["problems"] = compiler_problems(compiled, case)
    if case["outcome"] == "rejection" and (output.exists() or Path(str(output) + ".exe").exists()):
        result["problems"].append("rejected compilation left an output artifact")
    if result["problems"]:
        return result
    if case["outcome"] == "rejection":
        result["status"] = "PASS"
        return result
    # A Windows-hosted compiler invoked from MSYS can append .exe itself.
    if not output.exists() and Path(str(output) + ".exe").exists():
        output = Path(str(output) + ".exe")
    if not output.is_file() or output.is_symlink() or not os.access(output, os.X_OK):
        result["problems"] = ["fresh executable not produced"]
        return result
    # Warning-only diagnostic fixtures test compilation, not main()'s return.
    if "stdout" not in case:
        result["status"] = "PASS"
        return result
    executed = capture([str(output)], work, work / "run", args.run_timeout)
    result["run"] = executed
    if INTERNAL_FAILURE.search(executed["stdout"] + "\n" + executed["stderr"]):
        result["problems"] = ["runtime sanitizer/internal failure"]
        return result
    expected_exit = case.get("runtime_exit", 0)
    output_matches = (executed["stdout"].rstrip("\n") == case["stdout"] if case.get("stdout_exact")
                      else case["stdout"] in executed["stdout"])
    passed = (executed["category"] not in ("timeout", "crash", "launch-error") and
              executed["returncode"] == expected_exit and output_matches)
    if xfail_applies(case):
        rule = case["xfail"]
        if passed:
            result.update(status="XPASS", problems=["known ABI failure now passes; review/remove xfail"])
        elif executed["category"] == "rejection" and executed["returncode"] == rule["exit"] and \
                rule["stdout"] == executed["stdout"].rstrip("\n") and not executed["stderr"].strip():
            result.update(status="XFAIL", problems=[rule["reason"]])
        else:
            result["problems"] = ["failure does not match narrowly specified ABI xfail"]
    elif passed:
        result["status"] = "PASS"
    else:
        result["problems"] = ["runtime %s (exit=%s), expected exit %s and %sstdout %r" %
                              (executed["category"], executed["returncode"], expected_exit,
                               "exact " if case.get("stdout_exact") else "", case["stdout"])]
    return result


def positive_timeout(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("timeout must be finite and positive")
    return number


def file_hash(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_revision():
    try:
        revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                  check=True, timeout=5).stdout.strip()
        dirty = subprocess.run(["git", "status", "--porcelain", "--untracked-files=normal"],
                               cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                               check=True, timeout=5).stdout
        return {"revision": revision, "worktree_modified": bool(dirty)}
    except (OSError, subprocess.SubprocessError):
        return {"revision": None, "worktree_modified": None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("suite", choices=["diag", "cinterop", "signals"])
    parser.add_argument("compiler", nargs="?", default="./bin/stasha")
    parser.add_argument("--filter", default="", help="regular expression selecting case names; empty selection fails")
    parser.add_argument("--list", action="store_true", help="print explicit JSON case inventory without executing")
    parser.add_argument("--compile-timeout", type=positive_timeout,
                        default=os.environ.get("STASHA_TEST_COMPILE_TIMEOUT", "60"))
    parser.add_argument("--run-timeout", type=positive_timeout,
                        default=os.environ.get("STASHA_TEST_RUN_TIMEOUT", "15"))
    parser.add_argument("--artifacts", type=Path, help="parent directory for a fresh run directory")
    parser.add_argument("--keep-artifacts", action="store_true")
    parser.add_argument("--require-prerequisites", action="store_true", help="fail instead of skipping unavailable LLVM smoke")
    parser.add_argument("--support-lib", type=Path, default=ROOT / "tests/cinterop/build/libcinterop_support.a")
    parser.add_argument("--llvm-config", type=Path, default=ROOT / "build/llvm/bin/llvm-config")
    args = parser.parse_args()
    try:
        selector = re.compile(args.filter)
    except re.error as error:
        parser.error("invalid --filter: " + str(error))
    inventory = {"diag": diag_cases, "cinterop": interop_cases, "signals": signal_cases}
    cases = [case for case in inventory[args.suite]() if selector.search(case["name"])]
    if not cases:
        parser.error("filter selected no test cases")
    if args.list:
        print(json.dumps(cases, indent=2))
        return 0
    # Resolve relative compiler paths against the caller, before running from ROOT.
    found = shutil.which(args.compiler)
    args.compiler = str(Path(found or args.compiler).resolve())
    args.support_lib, args.llvm_config = args.support_lib.resolve(), args.llvm_config.resolve()
    if args.artifacts:
        args.artifacts.mkdir(parents=True, exist_ok=True)
    artifact_dir = Path(tempfile.mkdtemp(prefix="stasha-" + args.suite + "-", dir=args.artifacts)).resolve()
    print("=== Stasha %s test suite ===\nCompiler: %s\nArtifacts: %s" % (args.suite, args.compiler, artifact_dir), flush=True)
    started_at = datetime.now(timezone.utc).isoformat()
    results = []
    for case in cases:
        result = run_case(case, args, artifact_dir)
        results.append(result)
        print("  %s %s%s" % (result["status"], case["name"],
                              ": " + "; ".join(result["problems"]) if result["problems"] else ""), flush=True)
    counts = {status: sum(result["status"] == status for result in results)
              for status in ["PASS", "FAIL", "XFAIL", "XPASS", "SKIP"]}
    compiler_hash = file_hash(args.compiler) if Path(args.compiler).is_file() else None
    summary = {"suite": args.suite, "compiler": args.compiler, "compiler_sha256": compiler_hash,
               "host": platform.platform(), "python": sys.version, "started_at": started_at,
               "source": source_revision(),
               "counts": counts, "cases": results}
    (artifact_dir / "results.json").write_text(json.dumps(summary, indent=2) + "\n")
    print("=== Results: " + ", ".join("%d %s" % (count, status) for status, count in counts.items()) + " ===")
    failed = bool(counts["FAIL"] or counts["XPASS"])
    if failed or args.keep_artifacts or args.artifacts:
        print("Retained artifacts: " + str(artifact_dir))
    else:
        shutil.rmtree(artifact_dir)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
