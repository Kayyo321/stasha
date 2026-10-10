#!/usr/bin/env python3
"""Verify the source-built LLVM/LLD selection before qualification or release."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def run(*args, cwd=ROOT):
    return subprocess.check_output(list(map(str,args)), cwd=cwd, text=True,
                                   stderr=subprocess.STDOUT, timeout=30).strip()

def cache_entries(path):
    result = {}
    for line in path.read_text().splitlines():
        if line.startswith(('#','//')) or '=' not in line or ':' not in line.split('=',1)[0]: continue
        key_type, value = line.split('=',1)
        result[key_type.split(':',1)[0]] = value
    return result

def contained_path(path, build, label):
    resolved = path.resolve(strict=True)
    if not resolved.is_relative_to(build):
        raise ValueError(f'{label} resolves outside selected LLVM build: {resolved}')
    return resolved

def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()

def verify_clang(build, revision, projects):
    if 'clang' not in projects:
        raise ValueError('Pinned LLVM build does not include Clang')
    clang = contained_path(build/'bin/clang', build, 'Clang executable')
    if not clang.is_file() or clang.stat().st_size == 0 or not os.access(clang, os.X_OK):
        raise ValueError('Pinned Clang executable is missing, empty, or not executable')
    version = run(clang, '--version')
    first_line = version.splitlines()[0] if version else ''
    match = re.match(r'^clang version (\S+)(?:\s|$)', first_line)
    if not match or match.group(1) != '23.0.0git':
        raise ValueError(f'Expected pinned Clang 23.0.0git, got {first_line}')
    if not re.fullmatch(r'[0-9a-f]{40}', revision) or not re.search(
            r'(?<![0-9a-fA-F])' + re.escape(revision) + r'(?![0-9a-fA-F])', first_line):
        raise ValueError('Clang version does not contain the full pinned LLVM git revision')
    targets = re.findall(r'^Target: (\S+)\s*$', version, re.MULTILINE)
    if len(targets) != 1 or not re.fullmatch(r'x86_64-(?:[^\s-]+-)?linux-gnu', targets[0]):
        raise ValueError('Pinned Clang must target compatible Linux x86-64 GNU')
    resource_path = Path(run(clang, '--print-resource-dir'))
    if not resource_path.is_absolute():
        raise ValueError('Clang resource directory must be an absolute path inside the selected LLVM build')
    resource = contained_path(resource_path, build, 'Clang resource directory')
    if not resource.is_dir():
        raise ValueError('Clang resource directory is missing or not a directory')
    headers = {}
    for name in ('stdint.h', 'stddef.h', 'stdarg.h'):
        header = contained_path(resource/'include'/name, build, f'Clang resource header {name}')
        if not header.is_file() or header.stat().st_size == 0:
            raise ValueError(f'Clang resource header {name} is missing or empty')
        headers[name] = {'path':str(header), 'sha256':sha256(header)}
    return {'path':str(clang), 'sha256':sha256(clang), 'version':version,
            'target':targets[0], 'resource_dir':str(resource), 'resource_headers':headers}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--llvm-build', type=Path, default=ROOT/'build/llvm')
    ap.add_argument('--output', type=Path)
    ap.add_argument('--require-clang', action='store_true',
                    help='also verify pinned Linux x86-64 Clang and its resource headers')
    a=ap.parse_args()
    # A failed rerun must never leave a previous identity receipt looking current.
    if a.output:a.output.unlink(missing_ok=True)
    build=a.llvm_build.resolve();config=build/'bin/llvm-config'
    if not config.is_file():config=config.with_suffix('.exe')
    expected=run('git','rev-parse','HEAD:extlib/llvm')
    cache=cache_entries(build/'CMakeCache.txt')
    source=Path(cache.get('LLVM_SOURCE_DIR',cache.get('CMAKE_HOME_DIRECTORY',''))).resolve()
    actual=run('git','rev-parse','HEAD',cwd=source)
    if actual!=expected:raise ValueError(f'LLVM source revision {actual} differs from gitlink {expected}')
    dirty=run('git','status','--porcelain','--untracked-files=no',cwd=source)
    if dirty:raise ValueError('LLVM source has tracked modifications; qualification requires the exact gitlink')
    version=run(config,'--version')
    if version!='23.0.0git':raise ValueError(f'Expected pinned LLVM23.0.0git, got {version}')
    projects=set(cache.get('LLVM_ENABLE_PROJECTS','').split(';'))
    if 'lld' not in projects:raise ValueError('Pinned LLVM build does not include LLD')
    targets=run(config,'--targets-built').split()
    if 'X86' not in targets:raise ValueError('Pinned LLVM build lacks X86 target required by current qualification')
    libs=[build/'lib'/('liblld'+name+'.a') for name in ('Common','ELF','MachO','COFF','MinGW','Wasm')]
    if any(not p.is_file() or p.stat().st_size == 0 for p in libs):
        raise ValueError('Pinned LLD static libraries are missing or empty')
    receipt={'schema':1,'llvm_revision':actual,'llvm_version':version,
             'source_root':str(source),'build_root':str(build),'targets':targets,
             'projects':sorted(projects),'assertions':run(config,'--assertion-mode'),
             'rtti':run(config,'--has-rtti'),'host_target':run(config,'--host-target'),
             'llvm_config_sha256':hashlib.sha256(config.read_bytes()).hexdigest(),
             'cmake_cache_sha256':hashlib.sha256((build/'CMakeCache.txt').read_bytes()).hexdigest(),
             'lld_archives':{p.name:p.stat().st_size for p in sorted(libs)},
             'scope':'source/build identity receipt; not a reproducible-build or semantic-test claim'}
    if a.require_clang:
        receipt['clang'] = verify_clang(build, expected, projects)
    if a.output:a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt,indent=2))
if __name__=='__main__':
    try:main()
    except (OSError,ValueError,subprocess.SubprocessError) as e:raise SystemExit('toolchain verification failed: '+str(e))
