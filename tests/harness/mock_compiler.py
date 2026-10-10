#!/usr/bin/env python3
"""Deterministic fake compiler for oracle selftests; never used for qualification."""
import os
from pathlib import Path
import signal
import sys
import time

source = Path(sys.argv[sys.argv.index("build") + 1])
output = Path(sys.argv[sys.argv.index("-o") + 1])
mode = os.environ.get("STASHA_MOCK_MODE", "success")
name = source.stem
is_negative = name == "dedup_undefined_var" or "neg" in source.parts
if is_negative:
    message = "undefined variable 'missing'" if name == "dedup_undefined_var" else "unsupported bitfield"
    print("error[undefined]: " + message, file=sys.stderr)
    if mode != "no-location":
        print("  --> %s:9:5" % source, file=sys.stderr)
    print("error: aborting due to 1 previous error", file=sys.stderr)
    sys.stderr.flush()
    if mode == "timeout":
        time.sleep(10)
    if mode == "crash":
        os.kill(os.getpid(), signal.SIGKILL)
    if mode == "verifier":
        print("LLVM IR verification failed: invalid test IR", file=sys.stderr)
    if mode == "sanitizer":
        print("runtime error: signed integer overflow", file=sys.stderr)
    if mode == "output-on-rejection":
        output.write_text("bad output")
    sys.exit(0 if mode == "false-success" else 2 if mode == "wrong-exit" else 1)

if mode == "timeout":
    time.sleep(10)
if mode == "no-output":
    sys.exit(0)
if name.startswith("warn_") or name == "strict_promotes_warning":
    print("warning: unused variable 'y'", file=sys.stderr)
    print("  --> %s:9:5" % source, file=sys.stderr)
if mode == "false-success-with-error":
    print("error: simulated compiler error", file=sys.stderr)

payload = "#!/bin/bash\n"
if mode == "runtime-timeout":
    payload += "sleep 10\n"
if mode == "runtime-crash":
    payload += "kill -KILL $$\n"
if mode == "fixturefailure":
    payload += "echo 'fixture is deliberately wrong'\n"
elif "signals" in source.parts:
    text = "basic: code=42" if name == "basic" else "exit_block_ran"
    payload += "echo '%s'\n" % text
    if mode == "extra-output":
        payload += "echo unexpected\n"
else:
    payload += "echo '%s: ok'%s\n" % (name, " >&2" if mode == "stderr-only" else "")
payload += "exit %d\n" % (1 if mode == "runtime-failure" or name == "quit_from_exit" else 0)
output.write_text(payload)
output.chmod(0o755)
sys.exit(1 if mode == "compilerfailure" else 0)
