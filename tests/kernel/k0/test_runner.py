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
            with self.assertRaises(Failure): runner.run([sys.executable,"-c","import time; print('fake complete',flush=True); time.sleep(20)"],"timeout",timeout=1)
            self.assertEqual(len(list(Path(tmp).glob("*.json"))),2)
            self.assertIn("fake complete",(Path(tmp)/"0002-timeout.stdout").read_text())


# All additional component/tool records below are synthetic. No Stasha compiler,
# assembler, archive tool, linker, generated program or emulator is executed.
import contextlib
import io
import json
import os
import re
import shutil
import struct
from unittest import mock
import run as k0


def good_ir():
    return ('target triple = "x86_64-unknown-none-elf"\n' +
            '\n'.join(f'define void @k0_f{i}() #0 {{\n  ret void\n}}' for i in range(22)) +
            '\nattributes #0 = { noredzone noimplicitfloat nounwind "frame-pointer"="all" }\n' +
            'declare void @unused_hosted()\n')


def elf_bytes(debug=False, bad_name=None, bad_type=None, bad_flags=None, tls_symbol=False):
    """Small parser input, never assembled, linked or executed."""
    names = ["", ".shstrtab", ".text"] + ([".debug_info", ".debug_line"] if debug else [])
    if bad_name:
        names.append(bad_name)
    if tls_symbol:
        names.append(".symtab")
    strings = b"\0"
    offsets = [0]
    for name in names[1:]:
        offsets.append(len(strings)); strings += name.encode()+b"\0"
    payload = strings + b"\xc3"
    sections = [struct.pack("<IIQQQQIIQQ", 0,0,0,0,0,0,0,0,0,0)]
    sections.append(struct.pack("<IIQQQQIIQQ", offsets[1],3,0,0,64,len(strings),0,0,1,0))
    for index, name in enumerate(names[2:], 2):
        typ = 2 if name == ".symtab" else (bad_type if bad_type is not None and index == 2 else 1)
        flags = bad_flags if bad_flags is not None and index == 2 else 0
        offset = 64 + len(strings)
        size = 1 if name == ".text" else 0
        if name == ".symtab":
            offset = 64 + len(payload); size = 24
            payload += struct.pack("<IBBHQQ", 0,6,0,0,0,0)
        sections.append(struct.pack("<IIQQQQIIQQ", offsets[index],typ,flags,0,offset,size,0,0,1,24 if typ == 2 else 0))
    header = b"\x7fELF\x02\x01\x01"+b"\0"*9 + struct.pack("<HHIQQQIHHHHHH",1,62,1,0,0,64+len(payload),0,64,0,0,64,len(names),1)
    return header + payload + b"".join(sections)


def good_metadata(path, debug=False):
    names = ["", ".shstrtab", ".text"] + ([".debug_info", ".debug_line"] if debug else [])
    records = "".join(f"  Section {{\n    Name: {name} ({i})\n  }}\n" for i,name in enumerate(names))
    return (f"\nFile: {path}\nFormat: elf64-x86-64\nArch: x86_64\nAddressSize: 64bit\n"
            "ElfHeader {\n  Type: Relocatable (0x1)\n  Machine: EM_X86_64 (0x3E)\n}\nSections [\n" + records +
            "]\nRelocations [\n]\nSymbols [\n]\n")


def good_disassembly(path):
    return f"\n{path}:\tfile format elf64-x86-64\n\nDisassembly of section .text:\n0000 <k0_f0>:\n       0: c3    retq\n"


class MockEnvironment:
    def __init__(self, directory, mutate=None, full=False, archive_failure=False):
        self.root = Path(directory)/"repo"; self.here = self.root/"tests/kernel/k0"
        self.here.mkdir(parents=True)
        for file in k0.HERE.iterdir():
            if file.is_file(): shutil.copyfile(file, self.here/file.name)
        (self.root/"src").mkdir(); (self.root/"src/source.c").write_text("synthetic source\n")
        (self.root/"scripts").mkdir(); (self.root/"scripts/verify_llvm_toolchain.py").write_text("synthetic verifier\n")
        self.llvm = self.root/"build/llvm/bin"; self.llvm.mkdir(parents=True)
        (self.llvm.parent/"CMakeCache.txt").write_text("synthetic cache\n")
        for name in (*k0.INSPECTION_TOOLS, "llvm-config", "ld.lld", "llvm-ar", "cc", "objcopy"):
            (self.llvm/name).write_text("synthetic executable\n"); (self.llvm/name).chmod(0o755)
        self.compiler = self.root/"compiler"; self.compiler.write_text("synthetic compiler\n"); self.compiler.chmod(0o755)
        self.artifacts = Path(directory)/"artifacts"
        self.mutate = mutate; self.full = full; self.archive_failure = archive_failure; self.commands = []
        self.policy = json.loads((self.here/"policy.json").read_text())
        self.summary = None; self.output = ""; self.error = ""

    def receipt(self):
        return dict(schema=1, llvm_revision=self.policy["llvm_commit"], llvm_version="23.0.0git",
                    build_root=str(self.llvm.parent), source_root="synthetic llvm", targets=["X86", "AArch64"], projects=["clang", "lld"],
                    llvm_config_sha256=k0.sha(self.llvm/"llvm-config"), cmake_cache_sha256=k0.sha(self.llvm.parent/"CMakeCache.txt"),
                    lld_archives={"liblld"+name+".a":1 for name in ("Common","ELF","MachO","COFF","MinGW","Wasm")})

    def respond(self, command):
        self.commands.append(command)
        name = Path(command[0]).name; result = dict(stdout="", stderr="", returncode=0)
        if command[1:2] == ["--version"]:
            result["stdout"] = ("LLD "+self.policy["llvm_commit"] if name == "ld.lld" else
                                "LLVM (http://llvm.org/):\n  LLVM version 23.0.0git\n  Optimized build.\n")
        elif len(command)>1 and Path(command[1]).name == "verify_llvm_toolchain.py":
            result["stdout"] = json.dumps(self.receipt())
            Path(command[-1]).write_text(result["stdout"])
        elif name == "git":
            result["stdout"] = "a"*40+"\n" if command[1] == "rev-parse" else "synthetic patch error text\n"
        elif Path(command[0]) == self.compiler:
            mode = command[1]; path = Path(command[-1]); debug = "-g" in command
            if mode == "lib" and not self.full: raise AssertionError("object lane reached archive compiler")
            if mode == "lib" and self.archive_failure:
                path.write_bytes(elf_bytes())  # Partial archive must never override emission failure.
                result.update(returncode=7, stderr="archive rejected\n")
            else:
                path.write_bytes(good_ir().encode() if mode == "ir" else elf_bytes(debug))
            result["stdout"] = "info: debug selects O0\n" if debug else ""
        elif name == "opt":
            pass
        elif name == "llvm-nm":
            target = Path(command[-1])
            if target.name in ("component.o", "component.a"):
                lowered = target.parent.name in ("O2-nodebug", "O3-nodebug")
                result["stdout"] = "                 U __stasha_panic\n" + (" U memcpy\n U memmove\n U memset\n" if lowered else "")
            elif command[1:2] != ["--undefined-only"]:
                result["stdout"] = "0000 D k0_retention_cookie\n"
        elif name == "llvm-readobj":
            result["stdout"] = good_metadata(Path(command[-1]), "-debug" in command[-1])
        elif name == "llvm-objdump":
            result["stdout"] = good_disassembly(Path(command[-1]))
        elif self.full and name == "llvm-ar":
            result["stdout"] = "component.o\n" if command[1] == "t" else elf_bytes().decode("latin1")
        elif self.full and name == "ld.lld":
            target = Path(command[-1])
            if target.name == "missing-hook.elf":
                result.update(returncode=1, stderr="undefined symbol: __stasha_panic\n")
            elif target.name.startswith("missing-"):
                result.update(returncode=1, stderr="undefined symbol: "+target.stem.removeprefix("missing-")+"\n")
            elif target.name == "bad-model.elf":
                result.update(returncode=1, stderr="out of range\n")
            else: target.write_bytes(b"synthetic image")
        elif self.full and name == "cc":
            Path(command[-1]).write_bytes(b"synthetic host executable")
        elif self.full and name.startswith("host-"):
            # verify_host is separately covered by strict record unit tests.
            pass
        else:
            raise AssertionError("unexpected command: "+repr(command))
        if self.mutate: self.mutate(self, command, result)
        return result

    def invoke(self, extra=()):
        environment = self
        class FakeProcess:
            pid = 123
            def __init__(self, command, **kwargs):
                self.record = environment.respond(command); self.returncode = self.record["returncode"]
            def communicate(self, timeout=None):
                return self.record["stdout"].encode(), self.record["stderr"].encode()
        def compile_c(runner, cc, source, output, include, extra=()):
            environment.commands.append(["MOCK_COMPILE_C",str(source),str(output),*extra]); output.write_bytes(elf_bytes())
        out, err = io.StringIO(), io.StringIO()
        with contextlib.ExitStack() as stack:
            for attr, value in (("ROOT", self.root), ("HERE", self.here)):
                stack.enter_context(mock.patch.object(k0, attr, value))
            stack.enter_context(mock.patch.object(k0.subprocess, "Popen", FakeProcess))
            for name in ("prelink_negative_fixtures", "compile_c", "verify_host", "verify_boot", "elf_audit"):
                if self.full:
                    side_effect = compile_c if name == "compile_c" else None
                    stack.enter_context(mock.patch.object(k0, name, side_effect=side_effect, return_value={"status":"PASS"}))
                else:
                    stack.enter_context(mock.patch.object(k0, name, side_effect=AssertionError("excluded gate reached: "+name)))
            if self.full:
                # Binary archive extraction alone uses synthetic bytes; retain all other object audits.
                original = k0.relocatable_audit
                stack.enter_context(mock.patch.object(k0, "relocatable_audit", side_effect=lambda p, bits=64:
                    dict(status="PASS", sections=[]) if p.name == "archive-member.o" else original(p, bits)))
            else:
                stack.enter_context(mock.patch.object(k0.shutil, "which", side_effect=AssertionError("object lane resolved a host tool")))
            args = ["--compiler", str(self.compiler), "--llvm-bin",str(self.llvm), "--artifacts",str(self.artifacts)]
            args += (["--cc",str(self.llvm/"cc"),"--objcopy",str(self.llvm/"objcopy")] if self.full else ["--objects-only"])
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = k0.main(args+list(extra))
        self.output, self.error = out.getvalue(), err.getvalue()
        paths = sorted(self.artifacts.glob("run-*/summary.json"))
        if paths: self.summary = json.loads(paths[-1].read_text()); self.directory = paths[-1].parent
        return rc


class ObjectLane(unittest.TestCase):
    def test_default_matrix_command_boundary_and_truthful_flags(self):
        with tempfile.TemporaryDirectory() as tmp:
            env = MockEnvironment(tmp); self.assertEqual(env.invoke(), 0, env.error)
            summary = env.summary
            self.assertEqual(summary["requested_variants"], ["O0-nodebug","O0-debug","O2-nodebug","O2-debug","O3-nodebug","O3-debug"])
            self.assertEqual(summary["completed_variants"], summary["requested_variants"])
            self.assertTrue(summary["full_default_matrix"]); self.assertFalse(summary["full_k0_readiness"])
            self.assertEqual(set(summary["tools"]), set(k0.INSPECTION_TOOLS))
            compiler = [c for c in env.commands if c[0] == str(env.compiler)]
            self.assertEqual(len(compiler), 12); self.assertEqual([c[1] for c in compiler], ["obj","ir"]*6)
            for variant in summary["variants"]:
                level = variant["requested_optimization"]; debug = variant["debug"]
                self.assertEqual(variant["effective_optimization"], 0 if debug else level)
                cmds = [c for c in compiler if variant["name"] in c[-1]]
                self.assertEqual(len(cmds), 2)
                for command in cmds:
                    self.assertIn(f"-o={level}",command); self.assertEqual("-g" in command,debug)
                for stage in k0.OBJECT_STAGES: self.assertEqual(variant["stages"][stage]["status"], "PASS")
                self.assertEqual(set(variant["excluded_stages"]), set(k0.EXCLUDED_STAGES))
                self.assertTrue(all(x["status"] == "NOT_RUN" for x in variant["excluded_stages"].values()))
            for name in ("opt", "llvm-nm", "llvm-readobj", "llvm-objdump"):
                checks = [c for c in env.commands if Path(c[0]).name == name and c[1] != "--version"]
                self.assertEqual(len(checks),6)
                if name == "llvm-objdump": self.assertTrue(all("--no-debuginfod" in c for c in checks))
            self.assertIn("OBJECT/IR PASS",env.output); self.assertNotIn("COMPONENT PASS",env.output)
            self.assertNotIn('"RUNNING"', json.dumps(summary))

    def test_narrow_matrix(self):
        for options, names in ((["--opt","2","--debug","yes"],["O2-debug"]),
                               (["--opt","3,0","--debug","no"],["O3-nodebug","O0-nodebug"])):
            with tempfile.TemporaryDirectory() as tmp:
                env = MockEnvironment(tmp); self.assertEqual(env.invoke(options),0,env.error)
                self.assertEqual(env.summary["requested_variants"],names); self.assertFalse(env.summary["full_default_matrix"])

    def test_invalid_cli_before_any_output_or_process(self):
        for tail in (["--opt",""] ,["--opt","1"],["--opt","0,0"],["--opt","0,"],["--opt"," 0"],
                     ["--opt","00"],["--opt","x"],["--qemu","q"],["--require-boot"],
                     ["--qemu-data","data"],["--qemu-bios","bios"],["--qemu="],["--qemu-data="],["--qemu-bios="],["--qemu-mach","pc"],["--qemu-machine","pc-i440fx-10.0"],["--boot-repeats","2"]):
            with self.subTest(tail=tail), tempfile.TemporaryDirectory() as tmp:
                with mock.patch.object(k0.subprocess,"Popen",side_effect=AssertionError("process")), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    k0.main(["--objects-only","--llvm-bin","absent","--artifacts",tmp+"/new",*tail])
                self.assertFalse((Path(tmp)/"new").exists())

    def test_full_default_compatibility_and_archive_failure(self):
        for archive_failure in (False,True):
            with tempfile.TemporaryDirectory() as tmp:
                env = MockEnvironment(tmp,full=True,archive_failure=archive_failure)
                self.assertEqual(env.invoke(["--opt","2","--debug","no"]), int(archive_failure),env.error)
                self.assertEqual(env.summary["mode"],"full")
                self.assertEqual(env.summary["prelink_negative_fixtures"]["status"],"PASS")
                self.assertEqual(set(env.summary["tools"]), {"cc","objcopy","ld.lld","llvm-nm","llvm-readobj","llvm-objdump","llvm-ar","opt"})
                labels = [c["label"] for c in env.summary["commands"]]
                for gate in ("O2-nodebug-lib", "O2-nodebug-host-component", "O2-nodebug-low-component-panic0-link",
                             "O2-nodebug-high-component-panic0-link", "O2-nodebug-missing-hook", "O2-nodebug-bad-model",
                             "O2-nodebug-missing-memcpy","O2-nodebug-missing-memmove","O2-nodebug-missing-memset"):
                    self.assertIn(gate,labels)
                if not archive_failure: self.assertIn("O2-nodebug-host-archive",labels)
                else:
                    self.assertNotIn("COMPONENT PASS",env.output)
                    self.assertNotIn("O2-nodebug-archive",labels)
                    self.assertEqual(env.summary["variants"][0]["archive"],"FAIL")
                self.assertEqual(env.summary["boot"],"NOT_RUN")

    def test_missing_full_prerequisite_never_falls_back(self):
        with tempfile.TemporaryDirectory() as tmp:
            env = MockEnvironment(tmp,full=True); (env.llvm/"ld.lld").unlink()
            self.assertEqual(env.invoke(),1); self.assertEqual(env.summary["mode"],"full")
            self.assertFalse(any(c[0] == str(env.compiler) for c in env.commands))

    def test_failure_retains_exact_matrix_and_terminal_states(self):
        def failure(env, command, result):
            if command[0] == str(env.compiler) and command[1] == "ir": result["returncode"] = 7
        with tempfile.TemporaryDirectory() as tmp:
            env = MockEnvironment(tmp, mutate=failure); self.assertEqual(env.invoke(),1)
            self.assertEqual(len(env.summary["variants"]),6)
            self.assertEqual(env.summary["variants"][0]["status"],"FAIL")
            self.assertEqual(env.summary["variants"][0]["stages"]["ir_emission"]["status"],"FAIL")
            self.assertTrue(all(v["status"] == "NOT_RUN" for v in env.summary["variants"][1:]))
            self.assertNotIn('"RUNNING"',json.dumps(env.summary)); self.assertNotIn("OBJECT/IR PASS",env.output)
            self.assertTrue((env.directory/"O0-nodebug/component.ll").exists())
            self.assertFalse(any(Path(c[0]).name == "opt" and c[1] != "--version" for c in env.commands))

    def test_version_and_inspection_mutations_fail_closed(self):
        mutations = [
            ("version",dict(stdout="LLVM version 123.0.0git\n")),
            ("version",dict(stdout="LLVM version 23.0.0git-extra\n")),
            ("version",dict(returncode=7)), ("version",dict(returncode=-9)),
            ("version",dict(stderr="unexpected\n")),
            ("obj",dict(stdout="warning: synthetic\n")), ("ir",dict(stderr="error: synthetic\n")),
            ("opt",dict(stdout="unexpected\n")), ("opt",dict(returncode=1)),
            ("llvm-nm",dict(stdout="garbage ending in __stasha_panic\n")),
            ("llvm-nm",dict(stdout=" U malloc\n U __stasha_panic\n")),
            ("llvm-nm",dict(stdout=" U __stasha_panic\n U memcpy\n")),
            ("llvm-nm",dict(stderr="unexpected\n")),
            ("llvm-readobj",dict(stdout="elf64-x86-64\n")),
            ("llvm-objdump",dict(stdout="")),
        ]
        for kind, values in mutations:
            def mutate(env, command, result):
                if ((kind == "version" and command[1:2] == ["--version"]) or
                    (command[0] == str(env.compiler) and command[1] == kind) or
                    (Path(command[0]).name == kind and command[1:2] != ["--version"])):
                    result.update(values)
            with self.subTest(kind=kind,values=values), tempfile.TemporaryDirectory() as tmp:
                env = MockEnvironment(tmp, mutate=mutate); self.assertEqual(env.invoke(["--opt","0","--debug","no"]),1)
                self.assertEqual(env.summary["status"],"FAIL"); self.assertFalse(env.summary["full_k0_readiness"])
                self.assertNotIn('"RUNNING"',json.dumps(env.summary)); self.assertNotIn("OBJECT/IR PASS",env.output)

    def test_missing_invalid_object_tools_and_config(self):
        for name in (*k0.INSPECTION_TOOLS, "llvm-config"):
            for kind in ("absent", "directory", "empty", "nonexec", "broken", "escaped"):
                with self.subTest(name=name,kind=kind), tempfile.TemporaryDirectory() as tmp:
                    env=MockEnvironment(tmp); path=env.llvm/name; path.unlink()
                    if kind == "directory": path.mkdir()
                    elif kind == "empty": path.touch(); path.chmod(0o755)
                    elif kind == "nonexec": path.write_bytes(b"tool"); path.chmod(0o644)
                    elif kind == "broken": path.symlink_to(env.llvm/"missing")
                    elif kind == "escaped": path.symlink_to(env.compiler)
                    self.assertEqual(env.invoke(),1)
                    self.assertFalse(any(c[0] == str(env.compiler) for c in env.commands))
                    self.assertFalse(env.summary["full_k0_readiness"])

    def test_lowering_and_debug_metadata_mismatches(self):
        for options, kind in ((["--opt","2","--debug","no"], "helpers"),
                              (["--opt","3","--debug","yes"], "debug")):
            def mutate(env,command,result):
                if kind == "helpers" and Path(command[0]).name == "llvm-nm" and command[1] != "--version":
                    result["stdout"]=" U __stasha_panic\n"
                if kind == "debug" and Path(command[0]).name == "llvm-readobj" and command[1] != "--version":
                    result["stdout"]=good_metadata(Path(command[-1]),False)
            with self.subTest(kind=kind),tempfile.TemporaryDirectory() as tmp:
                env=MockEnvironment(tmp,mutate=mutate);self.assertEqual(env.invoke(options),1)

    def test_header_only_inspection_and_wrong_source_tree(self):
        for kind in ("metadata", "disassembly", "tree"):
            def mutate(env,command,result):
                if kind == "metadata" and Path(command[0]).name == "llvm-readobj" and command[1] != "--version":
                    result["stdout"]=re.sub(r"  Section \{.*?  \}\n", "", result["stdout"], flags=re.S)
                elif kind == "disassembly" and Path(command[0]).name == "llvm-objdump" and command[1] != "--version":
                    result["stdout"]=f"{command[-1]}: file format elf64-x86-64\n"
                elif kind == "tree" and command[:3] == ["git","rev-parse","--verify"]:
                    result["stdout"]="not-a-tree\n"
            with self.subTest(kind=kind),tempfile.TemporaryDirectory() as tmp:
                env=MockEnvironment(tmp,mutate=mutate);self.assertEqual(env.invoke(["--opt","0","--debug","no"]),1)

    def test_initial_summary_write_failure_is_nonzero(self):
        original=Path.write_text
        def fail(path,text,*args,**kwargs):
            if path.name == "summary.json": raise OSError("synthetic no storage")
            return original(path,text,*args,**kwargs)
        with tempfile.TemporaryDirectory() as tmp:
            env=MockEnvironment(tmp)
            with mock.patch.object(Path,"write_text",fail): self.assertEqual(env.invoke(),1)
            self.assertFalse(env.commands);self.assertNotIn("OBJECT/IR PASS",env.output)

    def test_bad_or_missing_verifier_receipt(self):
        for change in ("missing", "invalid", "revision", "stdout"):
            def mutate(env, command, result):
                if len(command)>1 and Path(command[1]).name == "verify_llvm_toolchain.py":
                    path=Path(command[-1])
                    if change == "missing": path.unlink()
                    elif change == "invalid": path.write_text("{}")
                    elif change == "revision":
                        receipt = env.receipt(); receipt["llvm_revision"]="b"*40
                        path.write_text(json.dumps(receipt)); result["stdout"]=path.read_text()
                    else: result["stdout"]="{}"
            with self.subTest(change=change), tempfile.TemporaryDirectory() as tmp:
                env=MockEnvironment(tmp,mutate=mutate); self.assertEqual(env.invoke(),1)

    def test_same_variant_output_drift_and_git_helper_boundary(self):
        for kind in ("object", "ir"):
            def mutate(env,command,result):
                if kind == "object" and Path(command[0]).name == "llvm-objdump" and command[1] != "--version":
                    path=Path(command[-1]);path.write_bytes(path.read_bytes()+b"changed after object audit")
                elif kind == "ir" and Path(command[0]).name == "opt" and command[1] != "--version":
                    path=Path(command[-1]);path.write_text(path.read_text()+"changed after verifier")
            with self.subTest(kind=kind),tempfile.TemporaryDirectory() as tmp:
                env=MockEnvironment(tmp,mutate=mutate)
                self.assertEqual(env.invoke(["--opt","0","--debug","no"]),1)
                self.assertIn("changed during inspection",env.summary["failure"])
                self.assertNotIn("OBJECT/IR PASS",env.output)
                for command in env.commands:
                    if command[:2] == ["git","diff"]:
                        self.assertIn("--no-ext-diff",command);self.assertIn("--no-textconv",command)

    def test_identity_and_snapshot_drift(self):
        for kind in ("compiler","tool","cache","source","snapshot","verifier","compiler_source","output"):
            done=[]
            def mutate(env, command, result):
                if Path(command[0]).name == "llvm-objdump" and command[1:2] != ["--version"] and not done:
                    paths={"compiler":env.compiler,"tool":env.llvm/"opt","cache":env.llvm.parent/"CMakeCache.txt",
                           "source":env.here/"component.sts","verifier":env.root/"scripts/verify_llvm_toolchain.py",
                           "compiler_source":env.root/"src/source.c",
                           "snapshot":Path(command[-1]).parents[1]/"sources/component.sts"}
                    if kind != "output": paths[kind].write_bytes(paths[kind].read_bytes()+b"changed\n")
                    else:
                        # Change an already inspected O0 artifact while inspecting O2.
                        if "O2-nodebug" not in command[-1]: return
                        path=Path(command[-1]).parents[1]/"O0-nodebug/component.o"; path.write_bytes(path.read_bytes()+b"changed")
                    done.append(True)
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as tmp:
                env=MockEnvironment(tmp,mutate=mutate); self.assertEqual(env.invoke(["--opt","0,2","--debug","no"]),1)
                self.assertEqual(env.summary["final_identity"]["status"],"FAIL")

    def test_fresh_artifact_failures_and_sentinel_preservation(self):
        for kind in ("missing","empty","symlink","directory","truncated","wrong_target","wrong_ir"):
            def mutate(env,command,result):
                if command[0] == str(env.compiler) and command[1] == ("ir" if kind == "wrong_ir" else "obj"):
                    path=Path(command[-1])
                    if kind == "wrong_ir": path.write_text(good_ir().replace("x86_64-unknown-none-elf","wrong-target")); return
                    path.unlink()
                    if kind == "empty": path.touch()
                    elif kind == "symlink": path.symlink_to(env.root/"sentinel")
                    elif kind == "directory": path.mkdir()
                    elif kind == "truncated": path.write_bytes(b"\x7fELF\x02\x01")
                    elif kind == "wrong_target": path.write_bytes(elf_bytes()[:18]+b"\xb7\0"+elf_bytes()[20:])
            with self.subTest(kind=kind),tempfile.TemporaryDirectory() as tmp:
                env=MockEnvironment(tmp,mutate=mutate); (env.root/"sentinel").write_text("preserve me")
                self.assertEqual(env.invoke(["--opt","0","--debug","no"]),1)
                self.assertEqual((env.root/"sentinel").read_text(),"preserve me")

    def test_terminal_receipt_write_failure_cannot_report_pass(self):
        original=Path.write_text
        def fail(path,text,*args,**kwargs):
            if path.name == "summary.json" and json.loads(text).get("status") == "PASS": raise OSError("synthetic disk full")
            return original(path,text,*args,**kwargs)
        with tempfile.TemporaryDirectory() as tmp:
            env=MockEnvironment(tmp)
            with mock.patch.object(Path,"write_text",fail): self.assertEqual(env.invoke(["--opt","0","--debug","no"]),1)
            self.assertNotIn("OBJECT/IR PASS",env.output); self.assertIn("terminal receipt",env.error)
            self.assertNotEqual(env.summary["status"],"PASS")


class IntegrityOracles(unittest.TestCase):
    def test_tool_file_selection(self):
        with tempfile.TemporaryDirectory() as tmp:
            base=Path(tmp); inside=base/"bin"; inside.mkdir(); good=inside/"tool"
            good.write_bytes(b"tool"); good.chmod(0o755)
            self.assertEqual(k0.file_identity(good,inside,True)["sha256"],k0.sha(good))
            link=inside/"link"; link.symlink_to(good)
            self.assertEqual(k0.file_identity(link,inside,True)["symlink"],str(good))
            for kind in ("absent","directory","empty","nonexec","broken","escaped"):
                path=inside/kind
                if kind == "directory": path.mkdir()
                elif kind == "empty": path.touch(); path.chmod(0o755)
                elif kind == "nonexec": path.write_bytes(b"data"); path.chmod(0o644)
                elif kind == "broken": path.symlink_to(inside/"missing")
                elif kind == "escaped":
                    outside=base/"outside"; outside.write_bytes(b"tool"); outside.chmod(0o755); path.symlink_to(outside)
                with self.subTest(kind=kind),self.assertRaises((Failure,OSError)):
                    k0.file_identity(path,inside,True)

    def test_ir_policy_preserved(self):
        k0.ir_audit(good_ir())
        for change in (lambda s:s.replace("x86_64-unknown-none-elf","wrong"),
                       lambda s:s.replace("define void @k0_f0() #0 {","declare void @missing()"),
                       lambda s:s.replace("noredzone", ""),lambda s:s.replace("noimplicitfloat", ""),
                       lambda s:s.replace("nounwind", ""),lambda s:s.replace('"frame-pointer"="all"', ""),
                       lambda s:s+"\n call void @malloc()\n",lambda s:s+"\n stack float\n",lambda s:s+" <4 x i32>\n"):
            with self.assertRaises(Failure): k0.ir_audit(change(good_ir()))

    def test_relocatable_parser_and_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/"object.o"; path.write_bytes(elf_bytes(True))
            audit=k0.relocatable_audit(path); self.assertIn(".debug_info",audit["sections"])
            k0.metadata_audit(good_metadata(path,True),path,audit["sections"],True)
            bads=[b"",elf_bytes()[:40],elf_bytes()[:-1],elf_bytes()[:4]+b"\x01"+elf_bytes()[5:],
                  elf_bytes()[:16]+b"\x02\0"+elf_bytes()[18:],elf_bytes()[:18]+b"\x03\0"+elf_bytes()[20:]]
            bads += [elf_bytes(bad_name=name) for name in (".tdata",".init_array",".eh_frame",".ctors")]
            bads += [elf_bytes(bad_flags=0x400),elf_bytes(bad_type=14),elf_bytes(tls_symbol=True)]
            for data in bads:
                path.write_bytes(data)
                with self.assertRaises(Failure):k0.relocatable_audit(path)
            for metadata in ("", "elf64-x86-64",good_metadata(Path("other"),True),good_metadata(path,False),
                             good_metadata(path,True).replace("Relocatable","Executable")):
                with self.assertRaises(Failure):k0.metadata_audit(metadata,path,[".debug_info",".debug_line"],True)

    def test_nonvacuous_disassembly_and_imports(self):
        path=Path("object.o");k0.disassembly_integrity(good_disassembly(path),path)
        for text in ("",f"{path}: file format elf64-x86-64\n",good_disassembly(Path("other")),
                     good_disassembly(path).replace("retq","fldl (%rax)")):
            with self.assertRaises(Failure):k0.disassembly_integrity(text,path)
        self.assertEqual(k0.imports_audit(" U __stasha_panic\n"),{"__stasha_panic"})
        for text in ("", "junk __stasha_panic", " X __stasha_panic", " U memcpy", " U malloc\n U __stasha_panic\n"):
            with self.assertRaises(Failure):k0.imports_audit(text)

    def test_fresh_paths_and_nonvacuous_matrix(self):
        with tempfile.TemporaryDirectory() as tmp:
            base=Path(tmp); path=base/"sentinel";path.write_bytes(b"preserve")
            with self.assertRaises(Failure):k0.fresh_output(path,base,before=True)
            other=base/"other";other.mkdir()
            with self.assertRaises(Failure):k0.fresh_output(other/"out",base,before=True)
            self.assertEqual(path.read_bytes(),b"preserve")
        variants=k0.requested_variants([0],[False],True,None)
        for state in ("PASS","FAIL","RUNNING","NOT_RUN"):
            v=copy.deepcopy(variants);v[0]["status"]=state
            with self.assertRaises(Failure):k0.matrix_complete(dict(requested_variants=["O0-nodebug"],variants=v))
        with self.assertRaises(Failure):k0.matrix_complete(dict(requested_variants=[],variants=[]))

    def test_spawn_signal_and_timeout_receipts(self):
        with tempfile.TemporaryDirectory() as tmp:
            runner=Runner(Path(tmp))
            with mock.patch.object(k0.subprocess,"Popen",side_effect=OSError("synthetic spawn denied")), self.assertRaises(Failure):
                runner.run(["synthetic"],"spawn")
            result=json.loads((Path(tmp)/"0001-spawn.json").read_text())
            self.assertEqual(result["category"],"spawn_error"); self.assertIsNone(result["returncode"])
            self.assertEqual(result["timeout_seconds"],60);self.assertIn("cwd",result)
            process=mock.Mock(returncode=-9);process.communicate.return_value=(b"",b"")
            with mock.patch.object(k0.subprocess,"Popen",return_value=process),self.assertRaises(Failure):
                runner.run(["synthetic"],"signal",expect=None)
            self.assertEqual(json.loads((Path(tmp)/"0002-signal.json").read_text())["category"],"signal")
            process=mock.Mock(returncode=0,pid=123);process.communicate.side_effect=[k0.subprocess.TimeoutExpired("synthetic",1),(b"fake pass",b"")]
            with mock.patch.object(k0.subprocess,"Popen",return_value=process),mock.patch.object(k0.os,"killpg"),self.assertRaises(Failure):
                runner.run(["synthetic"],"timeout",expect=None)
            result=json.loads((Path(tmp)/"0003-timeout.json").read_text())
            self.assertTrue(result["timed_out"]);self.assertEqual(result["category"],"timeout");self.assertEqual(result["returncode"],0)


if __name__ == "__main__": unittest.main()
