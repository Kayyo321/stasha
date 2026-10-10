#!/usr/bin/env python3
"""Typed cheader C callbacks/factories, separate from opaque-pointer bridges."""
from __future__ import annotations
import argparse, hashlib, json, os, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
HEADER='''#ifndef STS_ABI_CALLBACKS_H
#define STS_ABI_CALLBACKS_H
#include <stdint.h>
typedef struct { int32_t x; int32_t y; } abi_pair;
typedef struct { int64_t x; int64_t y; int64_t z; } abi_large;
typedef abi_pair (*abi_pair_fn)(abi_pair);
typedef abi_large (*abi_large_fn)(abi_large);
abi_pair_fn abi_get_pair(void);
abi_large_fn abi_get_large(void);
abi_pair abi_call_pair(abi_pair_fn cb, abi_pair p);
abi_large abi_call_large(abi_large_fn cb, abi_large p);
abi_pair abi_call_inline(abi_pair (*cb)(abi_pair), abi_pair p);
#endif
'''
C='''#include "callbacks.h"
#include <stdio.h>
static abi_pair c_pair(abi_pair p) { p.x+=3; p.y+=5; return p; }
static abi_large c_large(abi_large p) { p.x+=3; p.y+=5; p.z+=7; return p; }
abi_pair_fn abi_get_pair(void) { return c_pair; }
abi_large_fn abi_get_large(void) { return c_large; }
abi_pair abi_call_pair(abi_pair_fn cb,abi_pair p) { return cb(p); }
abi_large abi_call_large(abi_large_fn cb,abi_large p) { return cb(p); }
abi_pair abi_call_inline(abi_pair (*cb)(abi_pair),abi_pair p) { return cb(p); }
extern int32_t typed_callback_probe(void);
int main(void) { int r=typed_callback_probe(); if(r) { printf("typed callback failure: %d\\n",r); return r; } puts("typed C callback typedefs, inline parameters and factories: passed"); return 0; }
'''
STS='''@[[freestanding, crash: disable]];
cheader "callbacks.h" search "SEARCH";
@[[abi: c]] ext fn sts_pair(stack abi_pair p): abi_pair { p.x += 3; p.y += 5; ret p; }
@[[abi: c]] ext fn sts_large(stack abi_large p): abi_large { p.x += 3; p.y += 5; p.z += 7; ret p; }
@[[abi: c]] ext fn typed_callback_probe(void): i32 {
    stack abi_pair p = .{10,20};
    stack abi_pair_fn small = abi_get_pair();
    p = small(p);
    if p.x != 13 || p.y != 25 { ret 1; }
    p = abi_call_pair(sts_pair, p);
    if p.x != 16 || p.y != 30 { ret 2; }
    p = abi_call_inline(&sts_pair, p);
    if p.x != 19 || p.y != 35 { ret 3; }
    stack abi_large q = .{100,200,300};
    stack abi_large_fn large = abi_get_large();
    q = large(q);
    if q.x != 103 || q.y != 205 || q.z != 307 { ret 4; }
    q = abi_call_large(&sts_large, q);
    if q.x != 106 || q.y != 210 || q.z != 314 { ret 5; }
    ret 0;
}
'''
def invoke(cmd,env=None):
    p=subprocess.run(list(map(str,cmd)),cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=120,env=env)
    if p.returncode: raise AssertionError(f'{cmd}: {p.returncode}\n{p.stdout}')
    return p.stdout

def main():
    ap=argparse.ArgumentParser();ap.add_argument('compiler',type=Path);ap.add_argument('--cc',default='clang');ap.add_argument('--out',type=Path);a=ap.parse_args()
    compiler=a.compiler.resolve();out=a.out or Path(tempfile.mkdtemp(prefix='stasha-callback-abi-'));out.mkdir(parents=True,exist_ok=True)
    (out/'callbacks.h').write_text(HEADER);(out/'oracle.c').write_text(C);(out/'callbacks.sts').write_text(STS.replace('SEARCH',str(out.resolve())))
    report={'compiler':str(compiler),'sha256':hashlib.sha256(compiler.read_bytes()).hexdigest(),'c_compiler':invoke([a.cc,'--version']),'runs':[]}
    for opt in (0,2):
        obj=out/f'callbacks-O{opt}.o';exe=out/f'callbacks-O{opt}'
        env=dict(os.environ,STS_DUMP_IR_PRE=str(out/f'pre-O{opt}.ll'))
        invoke([compiler,'obj',out/'callbacks.sts',f'-o={opt}','-o',obj],env)
        invoke([a.cc,f'-O{opt}',out/'oracle.c',obj,'-o',exe])
        result=invoke([exe]);print(f'O{opt}: {result}',end='');report['runs'].append({'opt':opt,'result':result})
    # Calling native aggregate functions through C descriptors must reject.
    bad=out/'native_callback.sts';bad.write_text(STS.replace('SEARCH',str(out.resolve())).replace('@[[abi: c]] ext fn sts_pair','ext fn sts_pair'))
    badobj=out/'native_callback.o'
    p=subprocess.run([str(compiler),'obj',str(bad),'-o',str(badobj)],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=120)
    (out/'native_callback.log').write_text(p.stdout)
    if p.returncode==0 or 'erase the calling convention' not in p.stdout or badobj.exists(): raise AssertionError(f'native aggregate callback was not rejected:\n{p.stdout}')
    report['native_callback_rejected']=True
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(f'Artifacts: {out}')
if __name__=='__main__':main()
