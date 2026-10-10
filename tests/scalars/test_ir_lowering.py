#!/usr/bin/env python3
"""Verify production scalar LLVM builders against the pinned LLVM libraries.

This checks construction and optimization verification, not frontend typing or
runtime behavior. Full compiler execution remains tests/scalars/run.py's job.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def checked(command, **kwargs):
    result = subprocess.run([str(x) for x in command], capture_output=True,
                            text=True, timeout=120, **kwargs)
    if result.returncode:
        raise RuntimeError(f'{command}: exit {result.returncode}\n{result.stderr}')
    return result.stdout


def run(build, cc, cxx):
    config = build / 'bin/llvm-config'
    version = checked([config, '--version']).strip()
    if not version.startswith('23.'):
        raise RuntimeError(f'pinned LLVM23 required, got {version}')
    with tempfile.TemporaryDirectory(prefix='stasha-scalar-ir-') as tmp:
        tmp = Path(tmp)
        objects = []
        for source in ('tests/scalars/scalar_ir_probe.c', 'src/common/common.c', 'src/ast/ast.c'):
            obj = tmp / (Path(source).stem + '.o')
            checked([cc, '-std=c2x', '-O1', '-g', '-ffunction-sections', '-fdata-sections',
                     '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'extlib/llvm/llvm/include'),
                     '-I' + str(build / 'include'), '-c', ROOT / source, '-o', obj])
            objects.append(obj)
        libs = shlex.split(checked([config, '--link-static', '--ldflags', '--libs', 'all', '--system-libs']))
        exe = tmp / 'probe'
        checked([cxx, '-Wl,--gc-sections', *objects, *libs, '-o', exe])
        invalid = subprocess.run([str(exe), '--invalid-type'], capture_output=True, text=True, timeout=5)
        assert invalid.returncode == 1 and 'integer constant requires an integer LLVM type' in invalid.stderr
        ir = checked([exe])
        path = tmp / 'probe.ll'
        path.write_text(ir)
        if re.search(r'\b(?:nsw|nuw|exact)\b', ir):
            raise AssertionError('unproven arithmetic flags in pre-optimization IR')
        functions = re.findall(r'^define \w+ @(\w+)\([^\n]*\) \{(.*?)^\}', ir, re.M | re.S)
        bodies = dict(functions)
        for width in (8, 16, 32, 64):
            for signed, prefix in ((True, 'i'), (False, 'u')):
                name = prefix + str(width)
                for op in ('add', 'sub', 'mul'):
                    want = f'@llvm.{"s" if signed else "u"}{op}.with.overflow.i{width}'
                    assert want in bodies[name + '_checked_' + op], want
                for op, instruction in (('div', 'sdiv' if signed else 'udiv'),
                                        ('rem', 'srem' if signed else 'urem'),
                                        ('gt', 'icmp sgt' if signed else 'icmp ugt'),
                                        ('right', 'ashr' if signed else 'lshr')):
                    assert instruction + f' i{width}' in bodies[name + '_' + op]
                assert '@__stasha_panic(i32 2)' in bodies[name + '_div']
                assert ('@__stasha_panic(i32 3)' in bodies[name + '_div']) == signed
        for level in range(4):
            checked([build / 'bin/opt', '-passes=default<O' + str(level) + '>,verify',
                     '-S', path, '-o', tmp / f'O{level}.ll'])
        print('PASS: 64 production LLVM scalar functions verify before/after O0–O3; widths, signedness, and guarded division checked. Frontend/runtime execution not tested.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--llvm-build', type=Path, default=os.environ.get('LLVM_BUILD'))
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    if args.llvm_build is None:
        parser.error('--llvm-build or LLVM_BUILD is required')
    run(args.llvm_build.resolve(), args.cc, args.cxx)
