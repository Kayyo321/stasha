#!/usr/bin/env python3
"""Build compiler probes with host ASan/UBSan; run defined sentinel buffers only."""
import argparse,hashlib,json,os,shlex,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--llvm-build',type=Path,required=True);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();e=a.evidence.resolve();e.mkdir(parents=True,exist_ok=True);llvm=a.llvm_build.resolve();rows=[]
def run(command,timeout=180):
 r=subprocess.run(list(map(str,command)),cwd=ROOT,capture_output=True,text=True,timeout=timeout,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1'})
 rows.append({'command':list(map(str,command)),'returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr});(e/'receipt.json').write_text(json.dumps(rows,indent=2)+'\n')
 if r.returncode:raise SystemExit(f'FAILED {command}: {r.stderr}')
 return r.stdout
flags=['-std=c2x','-DTesting','-D_GNU_SOURCE','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-I'+str(ROOT/'src'),'-I'+str(llvm/'include'),'-I'+str(ROOT/'extlib/llvm/llvm/include')]
shared=[]
for source in ['src/common/common.c','src/ast/ast.c','src/parser/parser.c','src/lexer/lexer.c','src/preprocessor/preprocessor.c']:
 obj=e/(Path(source).stem+'.o');run(['gcc',*flags,'-c',ROOT/source,'-o',obj]);shared.append(obj)
libs=shlex.split(run([llvm/'bin/llvm-config','--link-static','--ldflags','--libs','all','--system-libs']))
obj=e/'descriptor_defaults_unit.o';exe=e/'descriptor_defaults_unit';run(['gcc',*flags,'-c',ROOT/'tests/interfaces/descriptor_defaults_unit.c','-o',obj]);run(['g++','-fsanitize=address,undefined','-Wl,--gc-sections',obj,*shared,*libs,'-o',exe]);print(run([exe,e/'sentinel.ll']))
for opt in range(4):
 out=e/f'sentinel-O{opt}';run([llvm/'bin/clang',f'-O{opt}',e/'sentinel.ll','-o',out]);run([out])
print('PASS defined sentinel buffers at O0–O3; no descriptor dispatch or empty-source execution')
