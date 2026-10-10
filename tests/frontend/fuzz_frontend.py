#!/usr/bin/env python3
"""Small repeatable mutation corpus; crashes/timeouts are never valid rejection."""
import pathlib
import random
import subprocess
import sys
import tempfile

probe = pathlib.Path(sys.argv[1]).resolve()
rng = random.Random(0x535441534841)
seeds = [
    "mod fuzz; fn f(void): i32 { stack i32 x = 1; if x > 0 { ret x + 2; } else { ret 0; } }",
    "mod fuzz; type S: struct { u64 x; }; stack u64 value = 0xFFFFFFFFFFFFFFFF;",
    "mod fuzz; fn f(stack []u8 b): void { for stack i32 i = 0; i < 4; i++ { b[i] = 1; } }",
    "mod fuzz; int macro let value! = 1; fn f(void): i32 { ret value.!; }",
    "mod fuzz; int macro fn value! { (@a) => { @a }; } fn f(void): i32 { ret value.!(1); }",
]
tokens = ["(", ")", "[", "]", "{", "}", ";", ":", ",", "0x",
          "18446744073709551616", "/*", "*/", "!", "@", "stack", "fn", "?", '"', "[]"]
accepted = rejected = 0
with tempfile.TemporaryDirectory(prefix="stasha-frontend-fuzz-") as directory:
    source_file = pathlib.Path(directory) / "mutation.sts"
    for case in range(256):
        source = rng.choice(seeds)
        for _ in range(1 + rng.randrange(3)):
            pos = rng.randrange(len(source) + 1)
            operation = rng.randrange(3)
            if operation == 0:
                source = source[:pos] + rng.choice(tokens) + source[pos:]
            elif operation == 1:
                source = source[:pos] + source[pos + rng.randrange(1, 8):]
            else:
                source = source[:pos] + rng.choice(tokens) + source[pos + rng.randrange(1, 8):]
        source_file.write_text(source, encoding="utf-8")
        try:
            result = subprocess.run([str(probe), str(source_file)],
                                    capture_output=True, text=True, timeout=2)
        except subprocess.TimeoutExpired:
            sys.exit(f"FAIL: mutation {case} timed out\n{source}")
        if (result.returncode not in (0, 1)
                or "runtime error:" in result.stderr
                or "Sanitizer" in result.stderr
                or "parse_result=" not in result.stdout):
            sys.exit(f"FAIL: mutation {case}, exit {result.returncode}\n"
                     f"{source}\n{result.stderr}")
        if result.returncode == 0:
            accepted += 1
        else:
            rejected += 1
print(f"PASS: 256 deterministic mutations ({accepted} accepted, {rejected} rejected; no crash/timeout)")
