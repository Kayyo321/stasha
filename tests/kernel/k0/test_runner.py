#!/usr/bin/env python3
"""Harness self-tests are not compiler or boot qualification evidence."""
import copy
from pathlib import Path
import sys
import tempfile
import unittest
from run import (EXPECTED_CHECKSUM, EXPECTED_COUNT, Failure, Runner,
                 disassembly_audit, memory_shim_audit, verify_boot, verify_host)
from memory_expectation import expected_memory_record

class StrictRecords(unittest.TestCase):
    def boot(self, reason=0):
        start = f"K0_START run=fresh build=sha mode={reason:016x}\n"
        end = (f"K0_PANIC run=fresh build=sha reason={reason:016x}\n" if reason else
               f"K0_COMPLETE run=fresh build=sha status=0000000000000000 observations={EXPECTED_COUNT:016x} checksum={EXPECTED_CHECKSUM}\n")
        return dict(stdout=start+end, stderr="", returncode=((32+reason if reason else 1)<<1)|1, timed_out=False)
    def test_valid_boot_records(self):
        for reason in range(5): verify_boot(self.boot(reason), "fresh", "sha", reason)
    def test_boot_mutations_fail(self):
        good = self.boot()
        mutations = [dict(stdout=""), dict(stdout=good["stdout"].replace("fresh", "stale")),
            dict(stdout=good["stdout"].replace("build=sha", "build=old")), dict(returncode=0),
            dict(returncode=-11), dict(timed_out=True), dict(stderr="reset\n"),
            dict(stdout=good["stdout"]+"K0_PANIC\n"), dict(stdout=good["stdout"]*2),
            dict(stdout=good["stdout"].replace(EXPECTED_CHECKSUM,"0000000000000000")),
            dict(stdout=good["stdout"].splitlines()[1]+"\n"),
            dict(stdout=good["stdout"].replace("status=0000000000000000","status=0000000000000001"))]
        for mutation in mutations:
            with self.subTest(mutation=mutation), self.assertRaises(Failure):
                verify_boot(good | mutation,"fresh","sha")
    def test_wrong_panic_is_failure(self):
        for reason in range(1,5):
            with self.assertRaises(Failure): verify_boot(self.boot(reason),"fresh","sha",(reason%4)+1)
    def test_host_records(self):
        result=dict(stdout=f"K0_COMPLETE run=fresh build=sha status=0 observations={EXPECTED_COUNT} checksum={EXPECTED_CHECKSUM}\n",stderr="",returncode=0)
        verify_host(result,"fresh","sha")
        with self.assertRaises(Failure): verify_host(result,"other","sha")
        for reason in range(1,5):
            verify_host(dict(stdout="",stderr=f"K0_PANIC run=fresh build=sha reason={reason}\n",returncode=80+reason),"fresh","sha",reason)
    def test_machine_policy(self):
        disassembly_audit("/build/k0-results/object.o: file format elf64-x86-64")
        disassembly_audit("pushq %rbp\nsubq $0x20, %rsp\nmovq %rdi, -0x8(%rbp)")
        for bad in ("pxor %xmm0, %xmm0", "movq %rdi, -0x8(%rsp)", "movq %fs:0x28, %rax", "vzeroupper", "fldl (%rax)",
                    "fsqrt", "fcompp", "fxch %st(1)", "fst %st", "fyl2xp1", "fptan", "fwait",
                    "fnstcw (%rax)", "ldmxcsr (%rax)", "vldmxcsr (%rax)", "vstmxcsr (%rax)",
                    "fxsave64 (%rax)", "fxrstor64 (%rax)", "xsaveopt (%rax)", "xrstors64 (%rax)", "kxnorw %k1, %k1, %k1",
                    "movq %rdi, -0x8(%rsp,%rax,1)", "movq %rdi, -8(%esp)"):
            with self.subTest(bad=bad), self.assertRaises(Failure): disassembly_audit(bad)
    def test_memory_record_against_independent_model(self):
        self.assertEqual(expected_memory_record(), (EXPECTED_COUNT, EXPECTED_CHECKSUM))
    def test_memory_shim_leaf_policy(self):
        memory_shim_audit("0000 <memcpy>:\nmovb (%rsi), %al\njne 10 <memcpy+0x10>\nretq")
        for bad in ("callq 0 <memcpy>", "callq 0 <unrelated>",
                    "jmpq 0 <memset>", "R_X86_64_PLT32 memmove-0x4",
                    "R_X86_64_PC32 memcpy-0x4", "pxor %xmm0, %xmm0"):
            with self.subTest(bad=bad), self.assertRaises(Failure): memory_shim_audit(bad)
    def test_process_failure_timeout_and_retention(self):
        with tempfile.TemporaryDirectory() as tmp:
            runner=Runner(Path(tmp))
            with self.assertRaises(Failure): runner.run([sys.executable,"-c","raise SystemExit(7)"],"failure")
            with self.assertRaises(Failure): runner.run([sys.executable,"-c","import time; print('fake complete',flush=True); time.sleep(20)"],"timeout",timeout=.05)
            self.assertEqual(len(list(Path(tmp).glob("*.json"))),2)
            self.assertIn("fake complete",(Path(tmp)/"0002-timeout.stdout").read_text())

if __name__ == "__main__": unittest.main()
