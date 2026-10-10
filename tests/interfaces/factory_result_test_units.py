#!/usr/bin/env python3
"""Build Factory result source/lifetime/receipt probes with host sanitizers and pinned LLVM23 libraries."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--llvm-build',type=Path,default=os.environ.get('LLVM_BUILD'))
p.add_argument('--cc',default='gcc');p.add_argument('--cxx',default='g++')
p.add_argument('--artifacts',type=Path,required=True)
p.add_argument('--phase',choices=['gate-a','gate-b'],default='gate-a')
a=p.parse_args()
if a.llvm_build is None: p.error('--llvm-build or LLVM_BUILD is required')
a.artifacts.mkdir(parents=True,exist_ok=True)
build=a.llvm_build.resolve(); work=Path(tempfile.mkdtemp(prefix='factory-result-units-',dir=a.artifacts))
results=[]
def run(command):
    r=subprocess.run([str(x) for x in command],cwd=ROOT,text=True,capture_output=True,timeout=180,
                     env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1'})
    results.append({'command':[str(x) for x in command],'returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
    (work/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    if r.returncode: raise SystemExit(f'FAIL exit {r.returncode}: {command}\n{r.stderr}\n{r.stdout}')
    return r.stdout
assert run([build/'bin/clang','--version']).startswith('clang version 23.')
run([a.cc,'--version']);run([a.cxx,'--version'])
flags=['-DFACTORY_UNIT_EXPECT_PROMOTION='+('1' if a.phase=='gate-b' else '0'),'-std=c2x','-DTesting','-D_GNU_SOURCE','-O1','-g','-fsanitize=address,undefined',
       '-fno-sanitize-recover=all','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections',
       '-I'+str(ROOT/'src'),'-I'+str(build/'include'),'-I'+str(ROOT/'extlib/llvm/llvm/include')]
shared=[]
for source in ['src/common/common.c','src/ast/ast.c','src/parser/parser.c','src/lexer/lexer.c','src/preprocessor/preprocessor.c']:
    obj=work/(Path(source).stem+'.o');run([a.cc,*flags,'-c',ROOT/source,'-o',obj]);shared.append(obj)
libs=shlex.split(run([build/'bin/llvm-config','--link-static','--ldflags','--libs','all','--system-libs']))
for probe in ['factory_result_unit','factory_result_codegen_unit']:
    obj=work/(probe+'.o');exe=work/probe
    run([a.cc,*flags,'-c',ROOT/f'tests/interfaces/{probe}.c','-o',obj])
    run([a.cxx,'-fsanitize=address,undefined','-Wl,--gc-sections',obj,*shared,*libs,'-o',exe])
    print(run([exe,work/'repeated.ll']).strip())
print(f'Evidence: {work}')
