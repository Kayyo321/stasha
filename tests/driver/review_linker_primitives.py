#!/usr/bin/env python3
"""Test actual pinned-LLVM linker/audit code using bounded C/assembly fixtures.
This verifies linker startup and the object inspector, not Stasha codegen.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/harness'))
from compiler_suites import capture, INTERNAL_FAILURE

FIXTURES = {
    'scalar': ('unsigned long scalar(unsigned long x) { return x+1; }', 0, None),
    'memory': ('extern void *memset(void*,int,unsigned long); void clear(void *p,unsigned long n) { memset(p,0,n); }', 0, None),
    'panic': ('extern void __stasha_panic(unsigned); void fail(void) { __stasha_panic(1); }', 0, None),
    'malloc': ('extern void *malloc(unsigned long); void *address(void) { return (void*)malloc; }', 1, 'malloc'),
    'preinit': ('void init(void) {} void (*preinit)(void) __attribute__((section(".preinit_array"),used))=init;', 1, '.preinit_array'),
    'tls': ('__thread int value __attribute__((section(".custom_tls"))); int read_tls(void) { return value; }', 1, '.custom_tls'),
}
HOSTED = '''#include <stdio.h>
#include <string.h>
static int state;
__attribute__((constructor)) static void before(void) { state=17; puts("constructor"); }
__attribute__((destructor)) static void after(void) { puts("destructor"); }
int main(int argc,char **argv) { if(state!=17 || argc!=2 || strcmp(argv[1],"argument")) return 3;
puts("main: argv and init correct"); return 37; }
'''


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('probe',type=Path)
    p.add_argument('--artifacts',type=Path)
    args=p.parse_args()
    probe=args.probe.resolve()
    if platform.system()!='Linux' or platform.machine() not in ('x86_64','aarch64'):
        p.error('requires native x86-64/AArch64 GNU/Linux and a C compiler')
    work=args.artifacts.resolve() if args.artifacts else Path(tempfile.mkdtemp(prefix='stasha-linker-review-'))
    work.mkdir(parents=True,exist_ok=True)
    if list(work.iterdir()): p.error('artifact directory must be fresh and empty')
    cc=os.environ.get('CC','cc'); results=[]
    def run(name,command,expected=0,contains=None,stdout=None,env=None):
        r=capture(command,ROOT,work/name,30,extra_env=env)
        errors=[]; text=r['stderr']+'\n'+r['stdout']
        if r['returncode']!=expected or r['category'] in ('timeout','crash','launch-error'):
            errors.append(f"expected exit {expected}, got {r['category']}/{r['returncode']}")
        if INTERNAL_FAILURE.search(text): errors.append('internal/compiler/sanitizer failure')
        if contains and contains not in text: errors.append('missing '+contains)
        if stdout is not None and r['stdout']!=stdout: errors.append('stdout mismatch')
        results.append(dict(name=name,process=r,problems=errors))
        print(('FAIL: ' if errors else 'PASS: ')+name)
        return not errors
    for name,(source,expected,diagnostic) in FIXTURES.items():
        path=work/(name+'.c'); path.write_text(source)
        obj=work/(name+'.o')
        if run('compile_'+name,[cc,'-fno-builtin','-fno-asynchronous-unwind-tables','-fno-unwind-tables','-c',path,'-o',obj]):
            # K0 object audit is x86-64 only; do not claim AArch64 audit support.
            if platform.machine()=='x86_64': run('audit_'+name,[probe,'audit',obj],expected,diagnostic)
    path=work/'custom_init.S'; path.write_text('.section .custom_init,"aw",@init_array\n.quad 0\n.section .note.GNU-stack,"",@progbits\n')
    if run('compile_custom_init',[cc,'-c',path,'-o',work/'custom_init.o']) and platform.machine()=='x86_64':
        run('audit_custom_init',[probe,'audit',work/'custom_init.o'],1,'.custom_init')
    path=work/'hosted.c'; path.write_text(HOSTED)
    target=platform.machine()+'-unknown-linux-gnu'; output=work/'hosted'
    if run('compile_hosted',[cc,'-fPIC','-O0','-c',path,'-o',work/'hosted.o']):
        if run('link_hosted',[probe,'link',work/'hosted.o',output,target]):
            run('execute_hosted',[output,'argument'],37,stdout='constructor\nmain: argv and init correct\ndestructor\n')
        other='aarch64' if platform.machine()=='x86_64' else 'x86_64'
        for i,foreign in enumerate([other+'-unknown-linux-gnu',platform.machine()+'-unknown-none-elf',platform.machine()+'-unknown-linux-musl','x86_64-unknown-linux-gnux32']):
            run('reject_target_'+str(i),[probe,'link',work/'hosted.o',work/('foreign'+str(i)),foreign],1,'native 64-bit GNU/Linux ABI')
        run('reject_bad_cc',[probe,'link',work/'hosted.o',work/'badcc',target],1,'working native C compiler',env={'CC':'/bin/false'})
    report=dict(probe=str(probe),probe_sha256=hashlib.sha256(probe.read_bytes()).hexdigest(),results=results)
    (work/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Retained evidence: '+str(work))
    return int(any(r['problems'] for r in results))
if __name__=='__main__': raise SystemExit(main())
