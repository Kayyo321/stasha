#!/usr/bin/env python3
"""Verify the source-built LLVM/LLD selection before qualification or release."""
import argparse
import hashlib
import json
from pathlib import Path
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

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--llvm-build', type=Path, default=ROOT/'build/llvm')
    ap.add_argument('--output', type=Path)
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
    if a.output:a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt,indent=2))
if __name__=='__main__':
    try:main()
    except (OSError,ValueError,subprocess.SubprocessError) as e:raise SystemExit('toolchain verification failed: '+str(e))
