#!/usr/bin/env python3
"""Independent, fail-closed full-driver contract probes.

Requires the actual Stasha compiler under review. Source validator tests and
secondary LLVM demonstrations are not substitutes for running this script.
Every invocation gets a fresh directory; failures and provenance are retained.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests' / 'harness'))
from compiler_suites import capture, INTERNAL_FAILURE

TARGET = 'x86_64-unknown-none-elf'
SCALAR = 'mod review; ext fn scalar(stack u64 x): u64 { ret x; }\n'
MALLOC = 'mod review; ext fn address(void): void *rw { unsafe { ret &malloc; } }\n'
PREINIT = 'mod review; @[[section: ".preinit_array"]] const u64 entry = 1;\n'
ZERO = '''mod review;
ext fn clear(stack u8 *rw p, stack u64 n): void {
    stack u64 i = 0;
    while (i < n) { unsafe { p[i] = 0; } i++; }
}
'''


def cases():
    for opt in range(4):
        yield dict(name=f'scalar_obj_O{opt}', mode='obj', source=SCALAR, opt=opt,
                   target=TARGET, outcome='success', artifact='elf')
        yield dict(name=f'malloc_address_O{opt}', mode='obj', source=MALLOC, opt=opt,
                   target=TARGET, outcome='rejection', diagnostic=r"kernel object audit:.*unqualified runtime import 'malloc'")
    for target in ('x86_64-unknown-none-macho', 'x86_64-unknown-linux-macho',
                   'x86_64-unknown-none-coff'):
        yield dict(name='reject_' + target, mode='obj', source=SCALAR, opt=0,
                   target=target, outcome='rejection', diagnostic=r'unsupported target')
    yield dict(name='preinit_array_section', mode='obj', source=PREINIT, opt=0,
               target=TARGET, outcome='rejection', diagnostic=r'kernel object audit:.*forbidden hosted section')
    for mode in ('build', 'dylib'):
        yield dict(name='reject_kernel_' + mode, mode=mode, source=SCALAR, opt=0,
                   target=TARGET, outcome='rejection', diagnostic=r'kernel profile supports obj, ir, and lib only')
    yield dict(name='reject_kernel_check', mode='check', source=SCALAR, opt=0,
               target=TARGET, outcome='rejection', diagnostic=r'kernel.*(?:obj|editor|check)', editor=True)
    yield dict(name='optimized_memory_loop', mode='obj', source=ZERO, opt=2,
               target=TARGET, outcome='success', artifact='elf')
    yield dict(name='scalar_ir', mode='ir', source=SCALAR, opt=2,
               target=TARGET, outcome='success', artifact='ir')
    yield dict(name='scalar_archive', mode='lib', source=SCALAR, opt=2,
               target=TARGET, outcome='success', artifact='archive')


def run_case(case, compiler, directory, timeout):
    work = directory / case['name']
    work.mkdir()
    source = work / 'probe.sts'
    source.write_text(case['source'])
    output = work / 'artifact'
    if not case.get('editor'):
        output.write_bytes(b'STALE OUTPUT: must be invalidated or replaced\n')
    command = [str(compiler), case['mode'], str(source), '--kernel', '--target',
               case['target'], f"-o={case['opt']}", '-o', str(output)]
    result = capture(command, ROOT, work / 'compile', timeout)
    text = result['stderr'] + '\n' + result['stdout']
    problems = []
    if result['category'] != case['outcome']:
        problems.append(f"expected {case['outcome']}, got {result['category']}")
    if INTERNAL_FAILURE.search(text):
        problems.append('internal, verifier, or sanitizer failure is not an ordinary rejection')
    if case['outcome'] == 'rejection':
        if not re.search(case['diagnostic'], text, re.IGNORECASE):
            problems.append('missing expected diagnostic: ' + case['diagnostic'])
        if output.exists():
            problems.append('failed build left an output artifact')
        if Path(str(output) + '.stasha-abi').exists():
            problems.append('failed build left archive metadata')
    elif not output.is_file() or output.stat().st_size == 0:
        problems.append('successful build did not emit a nonempty regular artifact')
    else:
        data = output.read_bytes()
        if case['artifact'] == 'elf':
            if not (data[:7] == b'\x7fELF\x02\x01\x01' and data[16:20] == b'\x01\x00\x3e\x00'):
                problems.append('object is not little-endian ELF64 x86-64 ET_REL')
        elif case['artifact'] == 'ir':
            ir = data.decode(errors='replace')
            for required in (f'target triple = "{TARGET}"', 'noredzone', 'noimplicitfloat', 'nounwind', 'stasha.abi'):
                if required not in ir:
                    problems.append('IR missing required target/ABI policy: ' + required)
        elif case['artifact'] == 'archive':
            if not data.startswith(b'!<arch>\n'):
                problems.append('archive magic mismatch')
            metadata = Path(str(output) + '.stasha-abi')
            if not metadata.is_file() or metadata.read_text() != f'STASHA_ABI 7\nTARGET {TARGET}\n':
                problems.append('archive ABI sidecar missing or wrong')
    return dict(case=case, source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                compile=result, problems=problems, status='FAIL' if problems else 'PASS')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler', type=Path)
    parser.add_argument('--artifacts', type=Path)
    parser.add_argument('--timeout', type=float, default=60)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    if not compiler.is_file():
        parser.error(f'actual compiler not found: {compiler}')
    directory = args.artifacts.resolve() if args.artifacts else Path(tempfile.mkdtemp(prefix='stasha-driver-review-'))
    directory.mkdir(parents=True, exist_ok=True)
    if list(directory.iterdir()):
        parser.error('--artifacts must name a fresh empty directory')
    version = capture([str(compiler), '--version'], ROOT, directory / 'version', args.timeout)
    if version['category'] != 'success':
        raise SystemExit('compiler version query failed; see ' + str(directory))
    report = dict(compiler=str(compiler), compiler_sha256=hashlib.sha256(compiler.read_bytes()).hexdigest(),
                  source_revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  source_status=subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True),
                  llvm_revision=subprocess.check_output(['git', 'rev-parse', 'HEAD:extlib/llvm'], cwd=ROOT, text=True).strip(),
                  version=version, results=[])
    for case in cases():
        result = run_case(case, compiler, directory, args.timeout)
        report['results'].append(result)
        print(result['status'] + ': ' + case['name'])
        for problem in result['problems']:
            print('  ' + problem)
    (directory / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Retained evidence: ' + str(directory))
    return int(any(r['problems'] for r in report['results']))


if __name__ == '__main__':
    raise SystemExit(main())
