#!/usr/bin/env python3
"""Differential test of the production C constant engine under host UBSan.

This does not execute emitted LLVM programs and is not full P1 qualification.
"""
import argparse
from pathlib import Path
import random
import subprocess
import tempfile

from reference import TYPES, IntType, ScalarTrap, binary, boundaries

OPS = ('+', '-', '*', '+%', '-%', '*%', '+!', '-!', '*!', '/', '%', '&', '|', '^', '==', '!=', '<', '>', '<=', '>=')
REASONS = {'overflow': 1, 'division-by-zero': 2, 'division-overflow': 3, 'invalid-shift': 4}


def run(cc='cc', exhaustive=False):
    here = Path(__file__).resolve().parent
    rng = random.Random(0x535441534841)
    commands, expected = [], []

    def case(op, typ, left, right, right_type=None):
        rt = right_type or typ
        commands.append(f'{op} {typ.width} {int(not typ.signed)} {rt.width} {int(not rt.signed)} {left & typ.mask} {right & rt.mask}\n')
        try:
            value = binary(op, left, right, typ)
            is_bool = isinstance(value, bool)
            expected.append(f'ok {int(value) if is_bool else value & typ.mask} {int(is_bool)}')
        except ScalarTrap as trap:
            expected.append(f'trap {REASONS[str(trap)]}')

    for typ in TYPES:
        pairs = [(x, y) for x in boundaries(typ) for y in boundaries(typ)]
        pairs += [(typ.wrap(rng.getrandbits(typ.width)), typ.wrap(rng.getrandbits(typ.width))) for _ in range(1000)]
        if exhaustive and typ.width == 8:
            pairs = [(typ.wrap(x), typ.wrap(y)) for x in range(256) for y in range(256)]
        for x, y in pairs:
            for op in OPS:
                case(op, typ, x, y)
        for x in boundaries(typ):
            for rt in TYPES:
                for count in set(boundaries(rt) + (rt.wrap(typ.width - 1), rt.wrap(typ.width), rt.wrap(256))):
                    for op in ('<<', '>>'):
                        case(op, typ, x, count, rt)
    with tempfile.TemporaryDirectory(prefix='stasha-constant-engine-') as tmp:
        exe = Path(tmp) / 'probe'
        build = subprocess.run([cc, '-std=c2x', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all', str(here / 'constant_engine_probe.c'), '-o', str(exe)], capture_output=True, text=True, timeout=60)
        if build.returncode or build.stderr:
            raise RuntimeError(f'C probe build failed ({build.returncode}): {build.stderr}')
        actual = subprocess.run([str(exe)], input=''.join(commands), capture_output=True, text=True, timeout=120)
        if actual.returncode or actual.stderr:
            raise RuntimeError(f'C probe failed ({actual.returncode}): {actual.stderr}')
        lines = actual.stdout.splitlines()
        if len(lines) != len(expected):
            raise AssertionError(f'output count {len(lines)} != {len(expected)}')
        for index, (got, want) in enumerate(zip(lines, expected)):
            if got != want:
                raise AssertionError(f'case {index}: {commands[index].strip()}: {got!r} != {want!r}')
        print(f'PASS: {len(expected)} production C constant operations match Python; UBSan clean. LLVM execution not tested.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--exhaustive', action='store_true', help='all 8-bit pairs for every binary operation')
    args = parser.parse_args()
    run(args.cc, args.exhaustive)
