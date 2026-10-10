#!/usr/bin/env python3
"""Compile/link-only A1 freestanding imports, hook and DataLayout contracts."""
import argparse
import json
from pathlib import Path
import re
import tempfile
import run_access_contracts as access

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--compiler',required=True,type=Path)
    p.add_argument('--cc',default='clang');p.add_argument('--artifacts',type=Path);a=p.parse_args()
    a.compiler=a.compiler.resolve(strict=True);cc=access.scalar_runner.resolve_cc(a.cc)
    if a.artifacts:a.artifacts.mkdir(parents=True,exist_ok=True)
    work=Path(tempfile.mkdtemp(prefix='bounds-freestanding-',dir=a.artifacts));results=[]
    before=access.scalar_runner.sha256(a.compiler)
    def run(name,command):
        r=access.scalar_runner.capture(command,work,work/name,30);return r
    def check(name,problems,process=None):
        result={'name':name,'status':'FAIL' if problems else 'PASS','problems':problems,'process':process}
        results.append(result);print(result['status'],name,'; '.join(problems),flush=True)
    source=work/'checked.sts';source.write_text('@[[freestanding]]; mod checked; ext fn checked(stack i64 index): i32 { stack i32 a[2]=.{7,8}; ret a[index]; }\n')
    for opt in range(4):
        obj=work/f'checked-O{opt}.o';ir=work/f'checked-O{opt}.ll'
        for mode,path in [('obj',obj),('ir',ir)]:
            r=run(f'{mode}-O{opt}',[a.compiler,mode,source,'-o='+str(opt),'-o',path])
            check(f'{mode}-O{opt}',[] if r['category']=='exit' and r['returncode']==0 and path.is_file() else ['compile failed'],r)
        if not obj.exists() or not ir.exists():continue
        txt=ir.read_text();problems=[]
        if 'call void @__stasha_panic(i32 6)' not in txt:problems.append('missing exact reason6 call')
        if re.search(r'(?:call|invoke) [^\n]*@(?:printf|puts|malloc|realloc|__crash)' ,txt):problems.append('unexpected hosted call in IR')
        if 'declare void @__stasha_panic(i32)' not in txt:problems.append('hook must be external declaration')
        check('reason6-no-IO-O'+str(opt),problems)
        r=run('imports-O'+str(opt),['nm','-u',obj]);imports=[line.strip().split()[-1] for line in r['stdout'].splitlines() if line.strip()]
        check('only-panic-import-O'+str(opt),[] if r['returncode']==0 and imports==['__stasha_panic'] else ['unexpected imports '+str(imports)],r)
        missing=work/f'missing-O{opt}'
        r=run('missing-hook-O'+str(opt),[cc,'-nostdlib','-Wl,-e,checked',obj,'-o',missing])
        check('missing-hook-link-fails-O'+str(opt),[] if r['category']=='exit' and r['returncode']!=0 and '__stasha_panic' in r['stderr'] and not missing.exists() else ['missing hook did not fail cleanly'],r)
    hook=work/'hook.c';hook.write_text('_Noreturn void __stasha_panic(unsigned reason) {(void)reason; __builtin_trap();}\n')
    hookobj=work/'hook.o';r=run('build-hook',[cc,'-ffreestanding','-fno-stack-protector','-c',hook,'-o',hookobj])
    image=work/'with-hook';r=run('provided-hook',[cc,'-nostdlib','-Wl,-e,checked',work/'checked-O0.o',hookobj,'-o',image])
    check('provided-hook-links',[] if r['returncode']==0 and image.is_file() else ['hook linking failed'],r)
    raw=work/'raw.sts';raw.write_text('@[[freestanding]]; mod raw; ext fn raw(stack i64 *rw p, stack u64 hi): void { unsafe { stack []i64 _v=p[0:hi]; } }\n')
    for target,limit in [('i386-unknown-none-elf',268435455),('x86_64-unknown-none-elf',1152921504606846975)]:
        out=work/(target+'.ll');r=run(target,[a.compiler,'ir',raw,'--target',target,'-o=0','-o',out]);problems=[]
        if r['returncode']!=0 or not out.is_file():problems.append('cross-target IR failed')
        else:
            txt=out.read_text()
            if not re.search(r'icmp ugt i64 [^\n]*, '+str(limit),txt):problems.append('missing target allocation-stride offset limit')
            if '2147483647' not in txt or 'call void @__stasha_panic(i32 6)' not in txt:problems.append('missing descriptor bound or reason6')
            if txt.index('call void @__stasha_panic(i32 6)')>txt.index('getelementptr i64'):problems.append('pointer arithmetic emitted before guard')
        check('raw-offset-'+target,problems,r)
    summary={'compiler':str(a.compiler),'compiler_sha256':before,'compiler_unchanged':before==access.scalar_runner.sha256(a.compiler),'results':results}
    summary['status']='PASS' if summary['compiler_unchanged'] and all(r['status']=='PASS' for r in results) else 'FAIL'
    (work/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(summary['status'],work/'summary.json');return int(summary['status']!='PASS')
if __name__=='__main__':raise SystemExit(main())
