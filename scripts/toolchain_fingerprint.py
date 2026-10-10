#!/usr/bin/env python3
"""Record exact cache/build inputs; never use a broad LLVM cache fallback."""
import argparse
import hashlib
import json
import os
import pathlib
import platform
import shlex
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent

def capture(command):
    return subprocess.check_output(command, cwd=ROOT, text=True,
                                   stderr=subprocess.STDOUT).strip()

p = argparse.ArgumentParser()
p.add_argument('--github-output', action='store_true')
p.add_argument('--output', type=pathlib.Path)
a = p.parse_args()
inputs = {
    'schema': 1,
    'system': platform.platform(),
    'machine': platform.machine(),
    'gitlinks': capture(['git', 'ls-tree', 'HEAD', 'extlib/llvm', 'extlib/openssl',
                         'extlib/cjson', 'extlib/mongoose', 'extlib/sodium']),
    'llvm_checkout': capture(['git', '-C', 'extlib/llvm', 'rev-parse', 'HEAD']),
    'build_rules_sha256': hashlib.sha256((ROOT / 'Makefile').read_bytes()).hexdigest(),
    'compiler': capture(shlex.split(os.environ.get('CC', 'clang')) + ['--version']),
    'cxx': capture(shlex.split(os.environ.get('CXX', 'clang++')) + ['--version']),
    'cmake': capture(['cmake', '--version']),
    'ninja': capture(['ninja', '--version']),
    'llvm_targets': os.environ.get('LLVM_TARGETS', 'X86;AArch64'),
    'cmake_generator': os.environ.get('CMAKE_GENERATOR', 'default'),
    'environment_flags': {k: os.environ.get(k, '') for k in
                          ('CFLAGS','CXXFLAGS','LDFLAGS','LLVM_BUILD','LLVM_ENABLE_PROJECTS')},
}
expected = capture(['git', 'rev-parse', 'HEAD:extlib/llvm'])
if inputs['llvm_checkout'] != expected:
    p.exit(1, 'LLVM checkout differs from repository gitlink; refusing cache fingerprint\n')
encoded = json.dumps(inputs, sort_keys=True, indent=2) + '\n'
fingerprint = hashlib.sha256(encoded.encode()).hexdigest()
if a.output:
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(encoded)
if a.github_output:
    with open(os.environ['GITHUB_OUTPUT'], 'a') as out:
        out.write(f'key={fingerprint}\n')
print(fingerprint)
