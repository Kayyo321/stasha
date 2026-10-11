#!/usr/bin/env python3
"""Fail-closed K0 component/ELF/optional QEMU qualification. No external modules."""
from __future__ import annotations
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import uuid

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
EXPECTED_COUNT = 155689
EXPECTED_CHECKSUM = "5710ad21404bb36e"
MEMORY_HELPERS = {"memcpy", "memmove", "memset"}
ALLOWED_IMPORTS = {"__stasha_panic"} | MEMORY_HELPERS
LOW = 0x200000
HIGH = 0xffffffff80200000

class Failure(RuntimeError):
    pass

def sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

def require(condition, message):
    if not condition:
        raise Failure(message)

def verify_host(result, run_id, build_id, reason=0):
    out, err, rc = result["stdout"], result["stderr"], result["returncode"]
    require(not result.get("timed_out"), "host execution timed out")
    if reason:
        expected = f"K0_PANIC run={run_id} build={build_id} reason={reason}\n"
        require(rc == 80 + reason and out == "" and err == expected,
                f"host panic {reason} wrong status/record: {result}")
    else:
        expected = (f"K0_COMPLETE run={run_id} build={build_id} status=0 "
                    f"observations={EXPECTED_COUNT} checksum={EXPECTED_CHECKSUM}\n")
        require(rc == 0 and out == expected and err == "", f"host result mismatch: {result}")

def verify_boot(result, run_id, build_id, reason=0):
    require(not result.get("timed_out"), "QEMU timeout: no accepted completion")
    prefix = f"run={run_id} build={build_id}"
    start = f"K0_START {prefix} mode={reason:016x}\n"
    if reason:
        expected = start + f"K0_PANIC {prefix} reason={reason:016x}\n"
        code = 32 + reason
    else:
        expected = (start + f"K0_COMPLETE {prefix} status=0000000000000000 "
                    f"observations={EXPECTED_COUNT:016x} checksum={EXPECTED_CHECKSUM}\n")
        code = 1
    # isa-debug-exit reports (written_value << 1) | 1, never conventional zero.
    require(result["returncode"] == (code << 1) | 1, "wrong QEMU debug-exit status")
    require(result["stdout"] == expected and result["stderr"] == "",
            f"QEMU records differ, include extra output, or lack fresh sentinel: {result}")

class Runner:
    def __init__(self, directory):
        self.directory = directory
        self.index = 0
        self.commands = []
    def run(self, command, label, timeout=60, expect=0, cwd=None, binary_output=None):
        command = [str(x) for x in command]
        self.index += 1
        stem = f"{self.index:04d}-{re.sub('[^a-zA-Z0-9_.-]', '_', label)}"
        began = time.monotonic()
        timed_out = False
        spawn_error = None
        stdout = stderr = b""
        returncode = None
        try:
            proc = subprocess.Popen(command, cwd=cwd or ROOT, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, start_new_session=True)
            try:
                stdout, stderr = proc.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                os.killpg(proc.pid, signal.SIGKILL)
                stdout, stderr = proc.communicate()
            returncode = proc.returncode
        except OSError as exc:
            spawn_error = str(exc)
            stderr = str(exc).encode()
        category = ("spawn_error" if spawn_error else "timeout" if timed_out else
                    "signal" if returncode < 0 else "nonzero" if returncode else "success")
        result = dict(command=command, cwd=str(Path(cwd or ROOT).resolve()), timeout_seconds=timeout,
                      returncode=returncode, category=category, spawn_error=spawn_error,
                      stdout=stdout.decode("utf-8", "replace") if binary_output is None else "[binary output retained]",
                      stderr=stderr.decode("utf-8", "replace"), timed_out=timed_out,
                      elapsed_seconds=round(time.monotonic() - began, 6))
        self.commands.append(dict(label=label, log=stem + ".json", command=command,
                                  cwd=result["cwd"], category=category, returncode=returncode))
        (self.directory / (stem + ".stdout")).write_bytes(stdout)
        (self.directory / (stem + ".stderr")).write_bytes(stderr)
        (self.directory / (stem + ".json")).write_text(json.dumps(result, indent=2) + "\n")
        require(not spawn_error, f"{label} spawn failed: {spawn_error}; see {stem}")
        require(not timed_out, f"{label} timed out; see {stem}")
        require(returncode >= 0, f"{label} terminated by signal {-returncode}; see {stem}")
        if expect is not None:
            require(returncode == expect, f"{label} failed ({returncode}); see {stem}")
        if binary_output is not None:
            Path(binary_output).write_bytes(stdout)
        return result



FORBIDDEN_SECTION = re.compile(
    r"\.(?:tdata|tbss|preinit_array|init_array|fini_array|ctors|dtors|init|fini|"
    r"eh_frame|eh_frame_hdr|gcc_except_table|interp|dynamic|dynsym)(?:$|\.)")


def relocatable_audit(path, bits=64):
    """Inspect ET_REL metadata before --gc-sections or /DISCARD/ can hide it.

    Ordinary relocation/debug sections are allowed. TLS flags/symbol types and
    initializer-array section types are checked even under innocent names.
    """
    data = path.read_bytes()
    require(bits in (32, 64), "unsupported object class")
    require(data[:6] == b"\x7fELF" + bytes((2 if bits == 64 else 1, 1)),
            f"expected ELF{bits} little-endian relocatable: {path}")
    hfmt = "<HHIQQQIHHHHHH" if bits == 64 else "<HHIIIIIHHHHHH"
    require(len(data) >= 16 + struct.calcsize(hfmt), "truncated ELF header")
    header = struct.unpack_from(hfmt, data, 16)
    kind, machine, _, _, _, shoff, _, _, _, _, shsize, shnum, names_idx = header
    require(kind == 1 and machine == (62 if bits == 64 else 3), "wrong relocatable type/architecture")
    sfmt = "<IIQQQQIIQQ" if bits == 64 else "<IIIIIIIIII"
    require(shsize == struct.calcsize(sfmt) and shnum > 0 and names_idx < shnum,
            "malformed or unsupported relocatable section table")
    require(shoff + shnum * shsize <= len(data), "truncated section table")
    sections = [struct.unpack_from(sfmt, data, shoff+i*shsize) for i in range(shnum)]
    names = sections[names_idx]
    require(names[1] == 3 and names[4] + names[5] <= len(data), "bad section-name string table")
    name_data = data[names[4]:names[4]+names[5]]
    audited = []
    for sec in sections:
        require(sec[0] < len(name_data) and b"\0" in name_data[sec[0]:], "bad section name")
        name = name_data[sec[0]:].split(b"\0", 1)[0].decode("utf-8", "replace")
        typ, flags, offset, size = sec[1], sec[2], sec[4], sec[5]
        require(typ == 8 or offset + size <= len(data), "truncated section payload")
        require(not FORBIDDEN_SECTION.match(name), f"forbidden prelink section {name}")
        require(not flags & 0x400, f"TLS section flag before link: {name}")
        require(typ not in (6, 11, 14, 15, 16, 0x70000001), f"runtime section type before link: {name} ({typ})")
        if typ == 2:  # SHT_SYMTAB: reject STT_TLS independently of spelling/flags.
            entry_size = 24 if bits == 64 else 16
            require(sec[9] == entry_size and size % entry_size == 0, "bad symbol table size")
            info_offset = 4 if bits == 64 else 12
            for start in range(offset, offset+size, entry_size):
                require(data[start+info_offset] & 15 != 6, "TLS symbol type before link")
        audited.append(name)
    return dict(status="PASS", bits=bits, sha256=sha(path), sections=audited)


def prelink_negative_fixtures(runner, cc, linker, directory):
    fixtures = directory / "prelink-fixtures"
    fixtures.mkdir()
    cases = {
        "ctors": '.section .ctors,"aw",@progbits',
        "dtors": '.section .dtors,"aw",@progbits',
        "init_array": '.section .init_array,"aw",@init_array',
        "fini_array": '.section .fini_array,"aw",@fini_array',
        "preinit_array": '.section .preinit_array,"aw",@preinit_array',
        "eh_frame": '.section .eh_frame,"a",@progbits',
        "hidden_unwind": '.section .innocent,"a",@0x70000001',
        "gcc_except_table": '.section .gcc_except_table,"a",@progbits',
        "tdata": '.section .tdata,"awT",@progbits',
        "tbss": '.section .tbss,"awT",@nobits',
        "hidden_tls": '.section .innocent,"awT",@progbits',
        "hidden_init": '.section .innocent,"aw",@init_array',
        "hidden_tls_symbol": '.section .innocent,"aw",@progbits\n.type hidden,@tls_object\nhidden:',
    }
    results = []
    for bits in (32, 64):
        for name, section in cases.items():
            source = fixtures / f"{name}-{bits}.S"
            source.write_text('.text\n.global _fixture_entry\n_fixture_entry: ret\n' + section + '\n.zero 8\n')
            obj = source.with_suffix(".o")
            assembled = runner.run([cc, f"-m{bits}", "-c", source, "-o", obj], f"prelink-fixture-{name}-{bits}")
            require(assembled["stdout"] == assembled["stderr"] == "", "fixture assembler emitted diagnostics")
            try:
                relocatable_audit(obj, bits)
            except Failure as exc:
                results.append(dict(case=name, bits=bits, rejected=str(exc)))
            else:
                raise Failure(f"prelink accepted forbidden fixture {name}-{bits}")
        # Explicitly demonstrate that the production DISCARD pattern can hide EH
        # metadata. This bypass is only a negative fixture, never an accepted input.
        script = fixtures / f"discard-{bits}.ld"
        script.write_text('SECTIONS { . = 0x1000; .text : { *(.text) } /DISCARD/ : { *(.eh_frame*) } }\n')
        concealed = fixtures / f"concealed-eh-{bits}.elf"
        runner.run([linker, "-m", "elf_x86_64" if bits == 64 else "elf_i386", "-static",
                    "-e", "_fixture_entry", "-T", script, fixtures/f"eh_frame-{bits}.o", "-o", concealed],
                   f"prelink-demonstrate-discard-{bits}")
        require(b".eh_frame\0" not in concealed.read_bytes(), "EH concealment fixture did not discard metadata")
    return dict(status="PASS", rejected=results, demonstrated_eh_discard=[32,64])


def elf_audit(path, expected_entry, delta):
    data = path.read_bytes()
    require(data[:6] == b"\x7fELF\x02\x01", "expected ELF64 little-endian")
    header = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    kind, machine, _, entry, phoff, shoff, _, _, phsize, phnum, shsize, shnum, names_idx = header
    require(kind == 2 and machine == 62, "expected static x86-64 executable")
    require(entry == expected_entry, f"entry does not match selected VMA: {entry:x}")
    segments = []
    for i in range(phnum):
        typ, flags, off, va, pa, filesz, memsz, align = struct.unpack_from("<IIQQQQQQ", data, phoff + i * phsize)
        require(typ not in (2, 3, 7), "dynamic/interpreter/TLS segment forbidden")
        if typ == 1:
            require(flags in (4, 5, 6), "unexpected or RWX load segment")
            require(pa == va - delta and LOW <= pa < LOW + 0x80000, "unexpected virtual/physical mapping")
            require(memsz >= filesz and off + filesz <= len(data) and pa + memsz <= LOW + 0x80000, "malformed or oversized segment")
            require(align == 4096 and va % align == off % align, "bad load alignment")
            segments.append(dict(vaddr=hex(va), paddr=hex(pa), flags=flags, filesz=filesz, memsz=memsz))
    require(len(segments) == 3 and {s["flags"] for s in segments} == {4, 5, 6}, "missing code/rodata/data segment")
    require(any(int(s["vaddr"],16) <= entry < int(s["vaddr"],16)+s["filesz"] and s["flags"] == 5 for s in segments), "entry is outside file-backed executable code")
    ordered = sorted(segments, key=lambda s: int(s["paddr"],16))
    require(all(int(a["paddr"],16)+a["memsz"] <= int(b["paddr"],16) for a,b in zip(ordered,ordered[1:])), "overlapping load mappings")
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff+i*shsize) for i in range(shnum)]
    names = sections[names_idx]
    names_data = data[names[4]:names[4]+names[5]]
    for sec in sections:
        name = names_data[sec[0]:].split(b"\0",1)[0].decode()
        require(sec[1] not in (4, 9) or not sec[2] & 2, "allocated relocation section in final image")
        require(not FORBIDDEN_SECTION.match(name),
                f"forbidden runtime section {name}")
    return dict(entry=hex(entry), segments=segments)

def disassembly_audit(text):
    require(not re.search(r"%(?:xmm\d+|ymm\d+|zmm\d+|mm[0-7]|k[0-7]|tmm[0-7])\b|%st(?:\([0-7]\))?", text, re.I),
            "FP/SIMD register in machine code")
    # Cover x87 operations with no register operands as well as arithmetic,
    # comparisons, stack manipulation, transcendental and environment ops.
    require(not re.search(
        r"\b(?:vzeroupper|vzeroall|emms|femms|v?ldmxcsr|v?stmxcsr|xsave\w*|xrstor\w*|"
        r"f(?:2xm1|abs|add|bld|bst|chs|clex|cmov|com|cos|decstp|disi|div|eni|free|"
        r"iadd|icom|idiv|ild|imul|incstp|init|ist|isub|ld|mul|nclex|ninit|nop|"
        r"nsave|nst|patan|prem|ptan|rndint|rstor|save|scale|setpm|sin|sqrt|st|sub|"
        r"tst|ucom|wait|xam|xch|xrstor|xsave|xtract|yl2x)\w*|wait)\b", text, re.I),
        "FP/SIMD instruction in machine code")
    require(not re.search(r"-(?:0x[0-9a-f]+|[0-9]+)\(%[re]sp(?:,|\))", text, re.I),
            "negative stack-pointer access may use red zone")
    require(not re.search(r"%[fg]s:", text), "TLS/segment-relative code is outside profile")

def memory_shim_audit(text):
    disassembly_audit(text)
    require(not re.search(r"\bcallq?\b", text), "memory shim must remain leaf code")
    require(not re.search(r"R_X86_64_\w+\s+(?:memcpy|memmove|memset)(?:[-+\s]|$)", text),
            "memory shim recursively lowers to a memory helper")
    require(not re.search(r"\bjmpq?\b[^\n]*<(?:memcpy|memmove|memset)>", text),
            "memory shim tail-calls a memory helper")

def compile_c(runner, cc, source, output, include, extra=()):
    flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-fno-pie",
             "-fno-stack-protector", "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
             "-fno-builtin", "-fno-tree-loop-distribute-patterns", "-ffreestanding", "-mno-red-zone", "-mgeneral-regs-only",
             "-mcmodel=kernel", "-ffunction-sections", "-fdata-sections"]
    runner.run([cc, *flags, "-I", source.parent, "-I", include, *extra, "-c", source, "-o", output], "cc-"+output.name)
    audit = relocatable_audit(output)
    output.with_suffix(output.suffix+".prelink.json").write_text(json.dumps(audit, indent=2)+"\n")


CANDIDATE_FILES = ("tests/kernel/k0/run.py", "tests/kernel/k0/test_runner.py",
                   "tests/kernel/k0/README.md", "wiki/Kernel-Readiness-Status.md")
INSPECTION_TOOLS = ("opt", "llvm-nm", "llvm-objdump", "llvm-readobj")
OBJECT_STAGES = ("object_emission", "ir_emission", "ir_policy", "ir_verification",
                 "imports", "relocatable", "metadata", "disassembly")
EXCLUDED_STAGES = ("archive_validation", "archive_member_checks", "prelink_negative_fixtures",
                   "host_execution_abi", "memory_shim_execution", "runtime_counters_host",
                   "runtime_counters_boot", "missing_panic_links", "missing_helper_links",
                   "code_model_negative_links", "low_final_image", "high_final_image",
                   "boot", "debugger", "hardware")
DIAGNOSTIC = re.compile(r"\b(?:warning|error|fatal)\b", re.I)


def file_identity(path, within=None, executable=False):
    supplied = Path(path).absolute()
    resolved = supplied.resolve(strict=True)
    if within is not None:
        require(resolved.is_relative_to(Path(within).resolve(strict=True)),
                f"path escapes selected directory: {supplied}")
    require(resolved.is_file() and resolved.stat().st_size > 0,
            f"missing, nonregular or empty file: {supplied}")
    require(not executable or os.access(resolved, os.X_OK), f"non-executable tool: {supplied}")
    return dict(supplied_path=str(supplied), resolved_path=str(resolved),
                symlink=os.readlink(supplied) if supplied.is_symlink() else None,
                size=resolved.stat().st_size, mode=resolved.stat().st_mode,
                sha256=sha(resolved))


def fresh_output(path, directory, before=False):
    path = Path(path)
    require(path.parent.resolve(strict=True) == directory.resolve(strict=True), "escaped output path")
    if before:
        require(not os.path.lexists(path), f"pre-existing output forbidden: {path}")
        return
    require(not path.is_symlink(), f"symlink output forbidden: {path}")
    return file_identity(path, directory)


def successful(result, label, quiet=False, inspector=False, diagnostics=True):
    require(result.get("returncode") == 0 and not result.get("timed_out") and
            not result.get("spawn_error"), f"{label}: command did not complete normally")
    require(not diagnostics or not DIAGNOSTIC.search(result["stdout"] + result["stderr"]), f"{label}: warning/error diagnostic")
    require(not inspector or not result["stderr"], f"{label}: inspector stderr")
    require(not quiet or result["stdout"] == result["stderr"] == "", f"{label}: expected empty output")
    return result["stdout"]


def version_audit(result, expected):
    text = successful(result, "tool version", inspector=True)
    versions = re.findall(r"^\s*LLVM version (\S+)\s*$", text, re.M)
    require(versions == [expected], "tool version must contain exactly the pinned LLVM version line")


def imports_audit(text):
    records = text.splitlines()
    require(records and all(re.fullmatch(r"\s*U\s+[A-Za-z_][A-Za-z0-9_.$]*\s*", line)
                            for line in records), "malformed undefined-symbol report")
    imported = {line.split()[1] for line in records}
    require(imported <= ALLOWED_IMPORTS and "__stasha_panic" in imported, f"unqualified imports {imported}")
    return imported


def ir_audit(ir):
    require(re.findall(r'^target triple = "([^"\n]+)"$', ir, re.M) == ["x86_64-unknown-none-elf"], "wrong IR target")
    attributes = dict(re.findall(r"^attributes #(\d+) = \{([^\n]*)\}", ir, re.M))
    definitions = re.findall(r"^define [^\n]* #(\d+)(?: [^\n]*)? \{", ir, re.M)
    require(len(definitions) == 22, "unexpected component function inventory")
    for attribute_id in definitions:
        for attr in ("noredzone", "noimplicitfloat", "nounwind", '"frame-pointer"="all"'):
            require(attr in attributes.get(attribute_id, ""), f"missing function machine-policy IR attribute {attr}")
    calls = set(re.findall(r"\b(?:call|invoke)\b[^\n@]*@([a-zA-Z_][a-zA-Z0-9_.]*)", ir))
    require(all(symbol.startswith(("llvm.", "k0_")) or symbol in ALLOWED_IMPORTS for symbol in calls),
            f"unqualified live IR calls {calls}")
    require(not re.search(r"\b(?:float|double|fp128)\b|<\d+ x", ir), "FP/vector type in IR")


def metadata_audit(text, path, sections, debug):
    require(re.findall(r"^File: (.+)$", text, re.M) == [str(path)], "metadata names wrong input")
    for line in (r"^Format: elf64-x86-64$", r"^Arch: x86_64$", r"^AddressSize: 64bit$",
                 r"^\s*Type: Relocatable \(0x1\)$", r"^\s*Machine: EM_X86_64 \(0x3E\)$",
                 r"^Sections \[$", r"^Symbols \[$", r"^Relocations \[$"):
        require(re.search(line, text, re.M), "malformed relocatable metadata report")
    section_block = re.search(r"^Sections \[\n(.*?)^\]", text, re.M | re.S)
    require(section_block is not None, "incomplete metadata section list")
    recorded = re.findall(r"^\s*Name: (.*?) \([0-9]+\)$", section_block.group(1), re.M)
    require(recorded == sections and len(recorded) >= 2 and
            len(re.findall(r"^  Section \{$", section_block.group(1), re.M)) == len(sections),
            "metadata section inventory differs from relocatable input")
    require(text.count("{") == text.count("}") and text.count("[") == text.count("]"),
            "unbalanced or truncated metadata report")
    if debug:
        require(all(name in sections and re.search(r"^\s*Name: " + re.escape(name) + r"(?:\s|$)", text, re.M)
                    for name in (".debug_info", ".debug_line")), "debug object lacks DWARF info/line sections")


def disassembly_integrity(text, path):
    require(re.findall(r"^(.+):\s+file format elf64-x86-64\s*$", text, re.M) == [str(path)],
            "wrong or missing disassembly input/format")
    require(re.search(r"^Disassembly of section \S+:$", text, re.M) and
            re.search(r"^\s*[0-9a-fA-F]+:\s+(?:[0-9a-fA-F]{2}\s+)+\s*[A-Za-z][A-Za-z0-9]*\b", text, re.M),
            "empty or instruction-free component disassembly")
    disassembly_audit(text)


def source_files(directory):
    result = {}
    for path in sorted(directory.rglob("*")):
        if "__pycache__" in path.parts:
            continue
        require(not path.is_symlink(), f"source snapshot symlink forbidden: {path}")
        if path.is_file():
            result[str(path.relative_to(directory))] = sha(path)
    require(result, "empty source snapshot")
    return result


def not_run(reason):
    return dict(status="NOT_RUN", reason=reason)


def requested_variants(levels, debug_modes, objects_only, qemu):
    variants = []
    for level in levels:
        for debug in debug_modes:
            variant = dict(name=f"O{level}-{'debug' if debug else 'nodebug'}", status="NOT_RUN",
                reason="not yet reached", requested_optimization=level,
                effective_optimization=0 if debug else level, debug=debug,
                stages={stage:not_run("not yet reached") for stage in OBJECT_STAGES},
                boot="NOT_RUN", hardware="NOT_RUN", debugger="NOT_RUN", images=[])
            if objects_only:
                variant["excluded_stages"] = {stage:not_run("outside objects-only lane") for stage in EXCLUDED_STAGES}
                variant.update(archive="NOT_RUN", host="NOT_RUN")
            variants.append(variant)
    return variants


def start_stage(variant, stage):
    variant["stages"][stage] = dict(status="RUNNING")


def pass_stage(variant, stage):
    variant["stages"][stage] = dict(status="PASS")


def terminalize(value, reason):
    if isinstance(value, dict):
        for key, child in list(value.items()):
            if child == "RUNNING":
                value[key] = "FAIL"
                value["failure"] = reason
            elif isinstance(child, (dict, list)):
                terminalize(child, reason)
        if value.get("status") == "NOT_RUN" and value.get("reason") == "not yet reached":
            value["reason"] = "blocked: " + reason
    elif isinstance(value, list):
        for child in value:
            terminalize(child, reason)


def matrix_complete(summary):
    requested = summary["requested_variants"]
    variants = summary["variants"]
    require(requested and [v["name"] for v in variants] == requested, "requested matrix is empty or incomplete")
    require(all(v["status"] == "PASS" and all(v["stages"][s]["status"] == "PASS" for s in OBJECT_STAGES)
                for v in variants), "requested matrix has failed or uncompleted variants")


def validate_toolchain_receipt(receipt, policy, build, config, cache):
    require(isinstance(receipt, dict) and receipt.get("schema") == 1 and
            receipt.get("llvm_revision") == policy["llvm_commit"] and
            receipt.get("llvm_version") == policy["llvm_version"] and
            receipt.get("build_root") == str(build) and
            receipt.get("llvm_config_sha256") == config["sha256"] and
            receipt.get("cmake_cache_sha256") == cache["sha256"] and
            "X86" in receipt.get("targets", []) and "lld" in receipt.get("projects", []),
            "invalid or mismatched LLVM source/build receipt")
    require(all(isinstance(receipt.get("lld_archives", {}).get("liblld"+name+".a"), int) and
                receipt["lld_archives"]["liblld"+name+".a"] > 0
                for name in ("Common", "ELF", "MachO", "COFF", "MinGW", "Wasm")),
            "source/build receipt lacks configured LLD library identities")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    p.add_argument("--compiler", default=str(ROOT / "bin/stasha"))
    p.add_argument("--llvm-bin", required=True)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--objcopy", default="objcopy")
    p.add_argument("--artifacts", default=str(ROOT / "build/k0-results"))
    p.add_argument("--opt", default="0,2,3")
    p.add_argument("--debug", choices=("both", "yes", "no"), default="both")
    p.add_argument("--qemu", help="pinned QEMU executable or versioned local launcher")
    p.add_argument("--qemu-machine", default="pc-i440fx-10.0")
    p.add_argument("--qemu-data", help="pinned QEMU data directory")
    p.add_argument("--qemu-bios", help="explicit pinned BIOS path")
    p.add_argument("--require-boot", action="store_true")
    p.add_argument("--boot-repeats", type=int, default=2)
    p.add_argument("--objects-only", action="store_true", help="emit and inspect component obj/IR only; no link or execution")
    p.add_argument("--source-tree", default="HEAD", help="Git tree baseline for the exact source-patch receipt (default: HEAD)")
    raw_argv = list(sys.argv[1:] if argv is None else argv)
    args = p.parse_args(raw_argv)
    policy = None
    if not re.fullmatch(r"[023](?:,[023])*", args.opt) or len(set(args.opt.split(","))) != len(args.opt.split(",")):
        p.error("optimization levels must be a nonempty unique selection from 0,2,3")
    levels = [int(v) for v in args.opt.split(",")]
    if args.objects_only and any(x.split("=", 1)[0] in
                                ("--qemu", "--require-boot", "--qemu-data", "--qemu-bios", "--qemu-machine", "--boot-repeats")
                                for x in raw_argv):
        p.error("--objects-only conflicts with explicit QEMU/boot options")
    if not 2 <= args.boot_repeats <= 8:
        p.error("boot repeats must be between two and eight")
    if args.require_boot and not args.qemu:
        p.error("--require-boot needs an explicitly pinned QEMU")
    debug_modes = [False, True] if args.debug == "both" else [args.debug == "yes"]
    root = Path(args.artifacts).resolve()
    root.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="run-", dir=root))
    runner = Runner(directory)
    run_id = uuid.uuid4().hex
    variants = requested_variants(levels, debug_modes, args.objects_only, args.qemu)
    summary = dict(status="RUNNING", mode="objects-only" if args.objects_only else "full",
                   run_id=run_id, full_k0_readiness=False,
                   started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                   environment={key:os.environ[key] for key in ("LD_LIBRARY_PATH", "QEMU_MODULE_DIR", "DEBUGINFOD_URLS") if key in os.environ},
                   scope="bounded experimental K0 component; no P6 compiler final-link mode, K1, hardware or whole-language claim",
                   boot="NOT_RUN", hardware="NOT_RUN", debugger="NOT_RUN", variants=variants,
                   requested_variants=[v["name"] for v in variants], completed_variants=[],
                   full_default_matrix=levels == [0, 2, 3] and debug_modes == [False, True],
                   final_identity=not_run("not yet reached"))
    if args.objects_only:
        summary["excluded_stages"] = {stage:not_run("outside objects-only lane") for stage in EXCLUDED_STAGES}
        summary["prelink_negative_fixtures"] = not_run("outside objects-only lane")
    summary_path = directory / "summary.json"
    def save():
        summary["commands"] = runner.commands
        summary["completed_variants"] = [v["name"] for v in variants if v["status"] in ("PASS", "FAIL")]
        summary_path.write_text(json.dumps(summary, indent=2)+"\n")
    print(f"Artifacts: {directory}", flush=True)
    try:
        save()
        policy = json.loads((HERE / "policy.json").read_text())
        compiler_info = file_identity(args.compiler, executable=True)
        compiler = Path(compiler_info["resolved_path"])
        build_id = compiler_info["sha256"]
        summary.update(compiler=str(compiler), compiler_sha256=build_id, compiler_identity=compiler_info, policy=policy)
        llvm_supplied = Path(args.llvm_bin).absolute()
        llvm = llvm_supplied.resolve(strict=True)
        llvm_selection = dict(supplied_path=str(llvm_supplied), resolved_path=str(llvm),
                              symlink=os.readlink(llvm_supplied) if llvm_supplied.is_symlink() else None)
        summary["llvm_bin_selection"] = llvm_selection
        require(llvm.is_dir() and llvm.name == "bin", "select the configured LLVM build/bin directory")
        build = llvm.parent
        config_path = llvm / "llvm-config"
        if not config_path.is_file():
            config_path = config_path.with_suffix(".exe")
        config_info = file_identity(config_path, llvm, executable=True)
        cache_info = file_identity(build / "CMakeCache.txt", build)
        verifier = ROOT / "scripts/verify_llvm_toolchain.py"
        identities = {"compiler":compiler_info, "llvm_config":config_info, "cache":cache_info,
                      "verifier":file_identity(verifier), "runner":file_identity(Path(__file__)),
                      "policy":file_identity(HERE / "policy.json")}
        tools = {name:str(llvm/name) for name in INSPECTION_TOOLS}
        if not args.objects_only:
            cc = shutil.which(args.cc) or args.cc
            objcopy = shutil.which(args.objcopy) or args.objcopy
            tools = {"cc":cc, "objcopy":objcopy, **{name:str(llvm/name) for name in
                     ("ld.lld", "llvm-nm", "llvm-objdump", "llvm-readobj", "llvm-ar", "opt")}}
        summary["initial_identities"] = identities
        summary["tools"] = {}
        for name, path in tools.items():
            identity = file_identity(path, llvm if name not in ("cc", "objcopy") else None, executable=True)
            identities["tool:"+name] = identity
            result = runner.run([path, "--version"], "version-"+name)
            if name in INSPECTION_TOOLS:
                version_audit(result, policy["llvm_version"])
            else:
                successful(result, "version-"+name, inspector=True)
            summary["tools"][name] = dict(**identity, version=result["stdout"], version_stderr=result["stderr"])
        if not args.objects_only:
            require(policy["llvm_commit"] in summary["tools"]["ld.lld"]["version"], "LLD revision does not match the exact pinned LLVM source")
        receipt_path = directory / "llvm-toolchain-receipt.json"
        summary["llvm_verifier_sha256"] = identities["verifier"]["sha256"]
        result = runner.run([sys.executable, verifier, "--llvm-build", build, "--output", receipt_path],
                           "verify-pinned-llvm-source-build")
        successful(result, "source/build verifier", inspector=True)
        fresh_output(receipt_path, directory)
        receipt = json.loads(receipt_path.read_text())
        validate_toolchain_receipt(receipt, policy, build, config_info, cache_info)
        require(json.loads(result["stdout"]) == receipt, "verifier output and retained receipt differ")
        summary["llvm_toolchain_receipt"] = receipt
        summary["verifier_subcommands"] = [
            dict(command=["git", "rev-parse", "HEAD:extlib/llvm"], cwd=str(ROOT)),
            dict(command=["git", "rev-parse", "HEAD"], cwd=receipt["source_root"]),
            dict(command=["git", "status", "--porcelain", "--untracked-files=no"], cwd=receipt["source_root"]),
            *[dict(command=[str(config_path), flag], cwd=str(ROOT)) for flag in
              ("--version", "--targets-built", "--assertion-mode", "--has-rtti", "--host-target")]]
        summary["llvm_toolchain_receipt_sha256"] = sha(receipt_path)
        snapshot = directory / "sources"
        original_sources = source_files(HERE)
        compiler_sources = source_files(ROOT / "src")
        shutil.copytree(HERE, snapshot, ignore=shutil.ignore_patterns("__pycache__"))
        snapshot_sources = source_files(snapshot)
        require(snapshot_sources == original_sources, "source snapshot differs from selected inputs")
        summary["source_sha256"] = snapshot_sources
        summary["compiler_source_sha256"] = compiler_sources
        summary["compiler_source_scope"] = "current source identities only; compiler build provenance is separate"
        summary["git_head"] = successful(runner.run(["git", "rev-parse", "HEAD"], "git-head"), "git HEAD", inspector=True).strip()
        source_tree = successful(runner.run(["git", "rev-parse", "--verify", args.source_tree+"^{tree}"], "git-source-tree"),
                                 "source tree", inspector=True).strip()
        require(re.fullmatch(r"[0-9a-f]{40}", source_tree), "invalid resolved source tree")
        summary["source_tree"] = dict(requested=args.source_tree, resolved=source_tree,
                                     scope="tree baseline plus candidate-file patch and selected input hashes; external publication binding and compiler build provenance are separate")
        patch = successful(runner.run(["git", "diff", "--no-ext-diff", "--no-textconv", "--binary", source_tree, "--", *CANDIDATE_FILES], "git-source-patch"), "source patch", inspector=True, diagnostics=False)
        summary["source_patch_files"] = list(CANDIDATE_FILES)
        summary["source_patch_sha256"] = hashlib.sha256(patch.encode()).hexdigest()
        summary["initial_identities"] = identities
        if args.qemu:
            qemu = str(Path(args.qemu).resolve())
            qversion = runner.run([qemu, "--version"], "qemu-version")
            summary["qemu"] = dict(path=qemu, sha256=sha(qemu), version=qversion["stdout"],
                machine=args.qemu_machine, cpu="qemu64", ram="64M", smp=1, accelerator="tcg,thread=single",
                bios=args.qemu_bios, bios_sha256=sha(args.qemu_bios) if args.qemu_bios else None,
                data=args.qemu_data)
            require(args.qemu_bios and args.qemu_data, "boot requires explicit pinned firmware and data paths")
            require(policy["qemu"]["version"] in qversion["stdout"] and sha(qemu) == policy["qemu"]["sha256"], "QEMU binary differs from qualified policy pin")
            require(sha(args.qemu_bios) == policy["qemu"]["bios_sha256"], "BIOS differs from qualified policy pin")
            require(args.qemu_machine == policy["qemu"]["machine"], "QEMU machine differs from policy pin")
        if not args.objects_only:
            summary["prelink_negative_fixtures"] = prelink_negative_fixtures(runner, cc, tools["ld.lld"], directory)
        debug_modes = [False, True] if args.debug == "both" else [args.debug == "yes"]
        for level in levels:
            for debug in debug_modes:
                name = f"O{level}-{'debug' if debug else 'nodebug'}"
                vdir = directory/name
                vdir.mkdir()
                if not args.objects_only:
                    (vdir/"run_config.h").write_text(f'#define K0_RUN_ID "{run_id}-{name}"\n#define K0_BUILD_ID "{build_id}"\n')
                variant = next(v for v in variants if v["name"] == name)
                variant.update(status="RUNNING", reason="attempted", boot="RUNNING" if args.qemu else "NOT_RUN")
                save()
                opts = ["--kernel","--target","x86_64-unknown-none-elf",f"-o={level}"] + (["-g"] if debug else [])
                variant["emitted_sha256"] = {}
                emissions = (("obj","o"),("ir","ll")) if args.objects_only else (("obj","o"),("ir","ll"),("lib","a"))
                for mode, suffix in emissions:
                    stage = "object_emission" if mode == "obj" else "ir_emission" if mode == "ir" else None
                    if stage:
                        start_stage(variant, stage)
                    out = vdir / f"component.{suffix}"
                    fresh_output(out, vdir, before=True)
                    result = runner.run([compiler,mode,snapshot/"component.sts",*opts,"-o",out],name+"-"+mode, expect=None if mode == "lib" else 0)
                    if mode == "lib" and result["returncode"] != 0:
                        variant["archive"] = "FAIL"
                        variant["archive_failure"] = result["stderr"]
                        continue
                    emitted = fresh_output(out, vdir)
                    successful(result, "compiler emission")
                    variant["emitted_sha256"][out.name] = emitted["sha256"]
                    if stage:
                        pass_stage(variant, stage)
                start_stage(variant, "ir_policy")
                ir_audit((vdir/"component.ll").read_text())
                pass_stage(variant, "ir_policy")
                start_stage(variant, "ir_verification")
                successful(runner.run([tools["opt"],"-passes=verify","-disable-output",vdir/"component.ll"], name+"-verify-ir"),
                           "IR verification", quiet=True)
                pass_stage(variant, "ir_verification")
                start_stage(variant, "imports")
                imports = successful(runner.run([tools["llvm-nm"],"--undefined-only",vdir/"component.o"],name+"-imports"),
                                     "object imports", inspector=True)
                imported = imports_audit(imports)
                variant["imports"] = sorted(imported)
                # This pinned -g path bypasses aggregate-to-libcall optimization.
                lowered = bool(level and not debug)
                expected_helpers = MEMORY_HELPERS if lowered else set()
                require(imported & MEMORY_HELPERS == expected_helpers,
                        f"actual memory imports differ from pinned lowering expectation: {imported}")
                pass_stage(variant, "imports")
                start_stage(variant, "relocatable")
                variant["prelink_component"] = relocatable_audit(vdir/"component.o")
                pass_stage(variant, "relocatable")
                variant["memory_helpers"] = dict(actual_imports=sorted(imported & MEMORY_HELPERS),
                    lowering="libcalls" if lowered else "scalarized", runtime_counters_host="NOT_RUN" if args.objects_only else "RUNNING",
                    runtime_counters_boot="RUNNING" if args.qemu else "NOT_RUN",
                    shim_leaf_audit="NOT_RUN", missing_helper_links={},
                    missing_helper_link_scope="NOT_RUN: objects-only" if args.objects_only else "required" if lowered else "not applicable: scalarized")
                if not args.objects_only and variant.get("archive") != "FAIL" and (vdir/"component.a").exists():
                    listing = runner.run([tools["llvm-ar"],"t",vdir/"component.a"],name+"-archive")["stdout"].splitlines()
                    require(len(listing) == 1, "component archive must contain one reviewed object")
                    archive_imports = runner.run([tools["llvm-nm"],"--undefined-only",vdir/"component.a"],name+"-archive-imports")["stdout"]
                    archive_symbols = {line.split()[-1] for line in archive_imports.splitlines() if line.strip() and not line.rstrip().endswith(":")}
                    require(archive_symbols == imported, "archive and object import boundaries differ")
                    runner.run([tools["llvm-ar"], "p", vdir/"component.a"], name+"-archive-member",
                               binary_output=vdir/"archive-member.o")
                    variant["prelink_archive_member"] = relocatable_audit(vdir/"archive-member.o")
                    archive_disasm = runner.run([tools["llvm-objdump"], "-dr", vdir/"archive-member.o"],
                                                name+"-archive-member-disassembly")["stdout"]
                    disassembly_audit(archive_disasm)
                    variant["archive"] = "PASS"
                start_stage(variant, "metadata")
                object_metadata = successful(runner.run([tools["llvm-readobj"],"--file-headers","--sections","--relocations","--symbols",vdir/"component.o"],name+"-object"),
                                             "object metadata", inspector=True)
                metadata_audit(object_metadata, vdir/"component.o", variant["prelink_component"]["sections"], debug)
                pass_stage(variant, "metadata")
                start_stage(variant, "disassembly")
                disasm = successful(runner.run([tools["llvm-objdump"],"--no-debuginfod","-dr",vdir/"component.o"],name+"-disassembly"),
                                    "object disassembly", inspector=True)
                disassembly_integrity(disasm, vdir/"component.o")
                for filename, digest in variant["emitted_sha256"].items():
                    require(fresh_output(vdir/filename, vdir)["sha256"] == digest,
                            "compiler output changed during inspection: " + filename)
                require(variant["prelink_component"]["sha256"] == variant["emitted_sha256"]["component.o"],
                        "prelink audit differs from emitted component")
                pass_stage(variant, "disassembly")
                if args.objects_only:
                    variant["output_sha256"] = {f.name:sha(f) for f in (vdir/"component.o", vdir/"component.ll")}
                    variant["status"] = "PASS"
                    save()
                    continue
                for src in ("reference.c", "abi_probe.S", "entry64.S", "baseline.c", "memory_helpers.c", "host.c"):
                    compile_c(runner,cc,snapshot/src,vdir/(Path(src).stem+".o"),vdir,
                              [f"-DK0_REQUIRE_MEMORY_HELPERS={int(lowered)}"])
                compile_c(runner,cc,snapshot/"reference.c",vdir/"reference-baseline.o",vdir,
                          ["-DK0_REQUIRE_MEMORY_HELPERS=-1"])
                helper_imports = runner.run([tools["llvm-nm"], "--undefined-only", vdir/"memory_helpers.o"],
                                            name+"-helper-imports")["stdout"]
                require(helper_imports == "", "memory shim imports a function or runtime")
                helper_disasm = runner.run([tools["llvm-objdump"], "-dr", vdir/"memory_helpers.o"],
                                           name+"-helper-disassembly")["stdout"]
                memory_shim_audit(helper_disasm)
                variant["memory_helpers"]["shim_leaf_audit"] = "PASS"
                if variant.get("archive") == "PASS":
                    for src in ("reference.c", "abi_probe.S"):
                        compile_c(runner,cc,snapshot/src,vdir/(Path(src).stem+"-archive.o"),vdir,["-DK0_ARCHIVE_ABI", f"-DK0_REQUIRE_MEMORY_HELPERS={int(lowered)}"])
                # C implementation first proves the differential harness independently.
                for implementation in (("baseline", "component", "archive") if variant.get("archive") == "PASS" else ("baseline", "component")):
                    exe = vdir/("host-"+implementation)
                    runner.run([cc,"-no-pie",vdir/"host.o",
                        vdir/("reference-archive.o" if implementation == "archive" else "reference-baseline.o" if implementation == "baseline" else "reference.o"),
                        vdir/("abi_probe-archive.o" if implementation == "archive" else "abi_probe.o"),
                        vdir/("component.a" if implementation == "archive" else implementation+".o"),vdir/"memory_helpers.o","-o",exe],name+"-host-link-"+implementation)
                    verify_host(runner.run([exe],name+"-host-"+implementation),run_id+"-"+name,build_id)
                    for reason in range(1,5):
                        verify_host(runner.run([exe,str(reason)],name+f"-host-{implementation}-panic{reason}",expect=None),run_id+"-"+name,build_id,reason)
                variant["host"] = "PASS"
                variant["memory_helpers"]["runtime_counters_host"] = "PASS"
                for layout, address, delta in (("low",LOW,0),("high",HIGH,HIGH-LOW)):
                    ldir = vdir/layout
                    ldir.mkdir()
                    script = (snapshot/"payload.ld.in").read_text().replace("@VMA@",hex(address)).replace("@DELTA@",hex(delta))
                    (ldir/"payload.ld").write_text(script)
                    image_status = dict(layout=layout, status="RUNNING", boot_runs=[])
                    variant["images"].append(image_status)
                    # All panic paths are booted for each optimization/debug/address variant.
                    for implementation in (("baseline","component") if args.qemu else ("component",)):
                        for reason in (range(5) if args.qemu else (0,)):
                            label = f"{name}-{layout}-{implementation}-panic{reason}"
                            idir = ldir/f"{implementation}-{reason}"
                            idir.mkdir()
                            boot_run_id = f"{run_id}-{label}"
                            (idir/"run_config.h").write_text(f'#define K0_RUN_ID "{boot_run_id}"\n#define K0_BUILD_ID "{build_id}"\n')
                            compile_c(runner,cc,snapshot/"boot.c",idir/"boot.o",idir,[f"-DK0_PANIC_MODE={reason}"])
                            objects = [vdir/"entry64.o",idir/"boot.o",vdir/("reference-baseline.o" if implementation == "baseline" else "reference.o"),vdir/"abi_probe.o",vdir/(implementation+".o"),vdir/"memory_helpers.o"]
                            image = idir/"payload.elf"
                            link = [tools["ld.lld"],"-m","elf_x86_64","-static","--gc-sections","--no-undefined","--fatal-warnings",
                                    "-T",ldir/"payload.ld",f"-Map={idir/'payload.map'}",*objects,"-o",image]
                            runner.run(link,label+"-link")
                            audit = elf_audit(image,address,delta)
                            require(runner.run([tools["llvm-nm"],"--undefined-only",image],label+"-undefined")["stdout"] == "", "final image has imports")
                            symbols = runner.run([tools["llvm-nm"],image],label+"-symbols")["stdout"]
                            require("k0_retention_cookie" in symbols,"KEEP retention failed")
                            disassembly_audit(runner.run([tools["llvm-objdump"],"-d",image],label+"-image-disassembly")["stdout"])
                            runner.run([tools["llvm-readobj"],"--file-headers","--program-headers","--sections",image],label+"-image-metadata")
                            (idir/"audit.json").write_text(json.dumps(audit,indent=2)+"\n")
                            if args.qemu:
                                raw = idir/"payload.bin"
                                runner.run([objcopy,"-O","binary",image,raw],label+"-raw")
                                runner.run([objcopy,"-I","binary","-O","elf32-i386","-B","i386",raw,idir/"payload32.o"],label+"-wrap")
                                runner.run([cc,"-m32","-c",f"-DK0_PAYLOAD_ENTRY={hex(address)}",snapshot/"boot32.S","-o",idir/"boot32.o"],label+"-bootstrap")
                                for name32 in ("payload32.o", "boot32.o"):
                                    audit32 = relocatable_audit(idir/name32, 32)
                                    (idir/(name32+".prelink.json")).write_text(json.dumps(audit32,indent=2)+"\n")
                                runner.run([tools["ld.lld"],"-m","elf_i386","-static","--no-undefined","-T",snapshot/"boot32.ld",
                                    f"-Map={idir/'boot.map'}",idir/"boot32.o",idir/"payload32.o","-o",idir/"boot.elf"],label+"-boot-link")
                                qcmd = [qemu,"-accel","tcg,thread=single","-m","64M","-smp","1","-cpu","qemu64",
                                    "-machine",args.qemu_machine,"-L",args.qemu_data,"-bios",args.qemu_bios,
                                    "-nodefaults","-vga","none","-display","none","-monitor","none","-serial","stdio","-no-reboot",
                                    "-device","isa-debug-exit,iobase=0xf4,iosize=0x04","-kernel",idir/"boot.elf"]
                                for repeat in range(args.boot_repeats):
                                    result = runner.run(qcmd,label+f"-qemu-{repeat}",timeout=15,expect=None)
                                    verify_boot(result,boot_run_id,build_id,reason)
                                    image_status["boot_runs"].append(dict(implementation=implementation,panic=reason,repeat=repeat,status="PASS"))
                    image_status["status"] = "PASS"
                # Missing-hook negative link: not a crash or unrelated error.
                missing = runner.run([tools["ld.lld"],"-m","elf_x86_64","-static","--no-undefined","-e","k0_checksum",
                    vdir/"component.o","-o",vdir/"missing-hook.elf"],name+"-missing-hook",expect=None)
                require(missing["returncode"] > 0 and "undefined symbol: __stasha_panic" in missing["stderr"], "missing panic hook was not rejected")
                require(not (vdir/"missing-hook.elf").exists(), "failed link left an image")
                # Scalarized O0/debug has no helper import: do not fabricate coverage.
                # At O2/O3 omit exactly one implementation while resolving all other
                # symbols, and require precisely that helper to be unresolved.
                for helper in sorted(expected_helpers):
                    omitted = vdir/f"without-{helper}.o"
                    compile_c(runner,cc,snapshot/"memory_helpers.c",omitted,vdir,
                              [f"-DK0_OMIT_{helper.upper()}"])
                    absent_image = vdir/f"missing-{helper}.elf"
                    absent = runner.run([tools["ld.lld"],"-m","elf_x86_64","-static","--no-undefined",
                        "-T",vdir/"low/payload.ld",vdir/"entry64.o",vdir/"low/component-0/boot.o",
                        vdir/"reference.o",vdir/"abi_probe.o",vdir/"component.o",omitted,"-o",absent_image],
                        name+f"-missing-{helper}",expect=None)
                    undefined = set(re.findall(r"undefined symbol: ([a-zA-Z_][a-zA-Z0-9_]*)",absent["stderr"]))
                    require(absent["returncode"] > 0 and undefined == {helper},
                            f"missing {helper} did not fail solely for the omitted helper: {absent['stderr']}")
                    require(not absent_image.exists(), "failed missing-helper link left an image")
                    variant["memory_helpers"]["missing_helper_links"][helper] = "PASS"
                # Kernel code-model signed-32-bit relocations must reject out-of-range VMAs.
                badscript = (snapshot/"payload.ld.in").read_text().replace("@VMA@",hex(0x4000200000)).replace("@DELTA@",hex(0x4000000000))
                (vdir/"bad-model.ld").write_text(badscript)
                bad = runner.run([tools["ld.lld"],"-m","elf_x86_64","-static","--no-undefined","-T",vdir/"bad-model.ld",
                    vdir/"entry64.o",vdir/"low/component-0/boot.o",vdir/"reference.o",vdir/"abi_probe.o",vdir/"component.o",vdir/"memory_helpers.o","-o",vdir/"bad-model.elf"],name+"-bad-model",expect=None)
                require(bad["returncode"] > 0 and "out of range" in bad["stderr"], "out-of-range kernel code-model image accepted")
                variant["status"] = "PASS" if variant.get("archive") == "PASS" else "FAIL"
                variant["boot"] = "PASS" if args.qemu else "NOT_RUN"
                variant["memory_helpers"]["runtime_counters_boot"] = "PASS" if args.qemu else "NOT_RUN"
                variant["output_sha256"] = {str(f.relative_to(vdir)):sha(f) for f in vdir.rglob("*") if f.is_file() and f.suffix in (".o",".ll",".a",".elf",".bin")}
                print(f"COMPONENT {variant["status"]} {name}: host, IR/object, low/high images" + (", repeated QEMU" if args.qemu else "; boot NOT RUN") + f"; archive {variant.get('archive', 'FAIL')}",flush=True)
                save()
        summary["final_identity"] = dict(status="RUNNING")
        require(llvm_supplied.resolve(strict=True) == llvm and
                (os.readlink(llvm_supplied) if llvm_supplied.is_symlink() else None) == llvm_selection["symlink"],
                "LLVM build/bin selection changed during qualification")
        for key, identity in identities.items():
            current = file_identity(identity["supplied_path"])
            require(current == identity, f"identity changed during qualification: {key}")
        require(source_files(HERE) == original_sources, "selected K0 sources changed during qualification")
        require(source_files(snapshot) == snapshot_sources, "copied source snapshot changed during qualification")
        require(source_files(ROOT / "src") == compiler_sources, "compiler sources changed during qualification")
        final_patch = successful(runner.run(["git", "diff", "--no-ext-diff", "--no-textconv", "--binary", source_tree, "--", *CANDIDATE_FILES], "git-source-patch-final"),
                                 "final source patch", inspector=True, diagnostics=False)
        require(hashlib.sha256(final_patch.encode()).hexdigest() == summary["source_patch_sha256"], "working-tree patch changed during qualification")
        final_receipt_path = directory / "llvm-toolchain-receipt-final.json"
        result = runner.run([sys.executable, verifier, "--llvm-build", build, "--output", final_receipt_path],
                           "verify-pinned-llvm-source-build-final")
        successful(result, "final source/build verifier", inspector=True)
        fresh_output(final_receipt_path, directory)
        final_receipt = json.loads(final_receipt_path.read_text())
        validate_toolchain_receipt(final_receipt, policy, build, config_info, cache_info)
        require(final_receipt == receipt and json.loads(result["stdout"]) == final_receipt,
                "LLVM source/build identity changed during qualification")
        summary["final_identities"] = {key:file_identity(identity["supplied_path"]) for key, identity in identities.items()}
        require(summary["final_identities"] == identities, "identity changed during final verification")
        for variant in variants:
            for relative, digest in variant.get("emitted_sha256", {}).items():
                output = directory / variant["name"] / relative
                require(fresh_output(output, output.parent)["sha256"] == digest, "emitted output changed: " + str(output))
            for relative, digest in variant.get("output_sha256", {}).items():
                output = directory / variant["name"] / relative
                require(not output.is_symlink() and sha(output) == digest, "output changed after inspection: " + str(output))
        summary["final_identity"] = dict(status="PASS")
        if not args.objects_only:
            require(all(v.get("archive") == "PASS" for v in summary["variants"]), "component library-mode qualification failed; see variant archive diagnostics")
        matrix_complete(summary)
        summary["status"] = "PASS"
        summary["boot"] = "PASS" if args.qemu else "NOT_RUN"
    except (Failure, OSError, ValueError, KeyError, TypeError, struct.error) as exc:
        summary["status"] = "FAIL"
        summary["failure"] = str(exc)
        terminalize(summary, str(exc))
        print(f"FAIL: {exc}",file=sys.stderr)
    summary["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    # Retain even partial/failed outputs as evidence, without following symlinks.
    try:
        summary["retained_artifacts"] = {}
        for variant in variants:
            vdir = directory / variant["name"]
            if not vdir.is_dir():
                continue
            for path in sorted(vdir.rglob("*")):
                if path.suffix not in (".o", ".ll", ".a", ".elf", ".bin"):
                    continue
                record = dict(kind="symlink" if path.is_symlink() else "file" if path.is_file() else "other")
                if record["kind"] == "file":
                    record.update(size=path.stat().st_size, sha256=sha(path))
                elif path.is_symlink():
                    record["target"] = os.readlink(path)
                summary["retained_artifacts"][str(path.relative_to(directory))] = record
        save()
    except OSError as exc:
        summary["status"] = "FAIL"
        summary["failure"] = f"terminal receipt could not be saved: {exc}"
        terminalize(summary, summary["failure"])
        print(f"FAIL: {summary['failure']}", file=sys.stderr)
        try:
            save()  # Best effort only; a failed PASS write can never return success.
        except OSError:
            pass
        return 1
    if summary["status"] == "PASS" and args.objects_only:
        print("OBJECT/IR PASS: " + ", ".join(summary["requested_variants"]) + "; all excluded stages NOT_RUN", flush=True)
    print(summary_path,flush=True)
    return 0 if summary["status"] == "PASS" else 1

if __name__ == "__main__":
    raise SystemExit(main())
