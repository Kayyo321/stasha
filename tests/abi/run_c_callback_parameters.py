#!/usr/bin/env python3
"""C array-parameter adjustment and fail-closed callback declarator tests."""
import argparse, hashlib, json, os, subprocess, tempfile
from pathlib import Path
HEADER = '''#include <stdint.h>
typedef int32_t row[3];
typedef row row_alias;
typedef const row_alias const_row;
typedef int32_t (*const_alias_fn)(const row_alias values);
typedef int32_t (*nested_const_fn)(const_row values);
const_alias_fn get_const_alias(void);
nested_const_fn get_nested_const(void);
typedef int32_t (*fixed_fn)(int32_t values[3]);
typedef int32_t (*open_fn)(int32_t values[]);
typedef int32_t (*alias_fn)(row_alias values);
typedef int32_t (*pointer_fn)(int32_t *values);
typedef int32_t *pointer_alias;
typedef int32_t (*pointer_alias_fn)(pointer_alias values);
fixed_fn get_fixed(void);
open_fn get_open(void);
alias_fn get_alias(void);
pointer_fn get_pointer(void);
pointer_alias_fn get_pointer_alias(void);
int32_t *get_values(void);
int32_t direct_array(int32_t values[3]);
int32_t direct_alias(row_alias values);
'''
C = '''#include "parameters.h"
#include <stdio.h>
static int32_t values[3] = {11, 22, 33};
static int32_t const_sum(const int32_t p[3]) { return p == values ? p[0]+p[1]+p[2] : -1; }
const_alias_fn get_const_alias(void) { return const_sum; }
nested_const_fn get_nested_const(void) { return const_sum; }
static int32_t sum(int32_t p[3]) { return p == values ? p[0]+p[1]+p[2] : -1; }
fixed_fn get_fixed(void) { return sum; }
open_fn get_open(void) { return sum; }
alias_fn get_alias(void) { return sum; }
pointer_fn get_pointer(void) { return sum; }
pointer_alias_fn get_pointer_alias(void) { return sum; }
int32_t *get_values(void) { return values; }
int32_t direct_array(int32_t p[3]) { return sum(p); }
int32_t direct_alias(row_alias p) { return sum(p); }
extern int32_t probe(void);
int main(void) { int r=probe(); if(r) { printf("parameter failure: %d\\n",r); return r; } puts("array parameter pointers and typedef aliases: passed"); }
'''
STS = '''@[[freestanding, crash: disable]];
cheader "parameters.h" search "SEARCH";
@[[abi: c]] ext fn probe(void): i32 {
 stack let values = get_values();
 stack const_alias_fn f = get_const_alias();
 stack nested_const_fn g = get_nested_const();
 stack fixed_fn a = get_fixed();
 stack open_fn b = get_open();
 stack alias_fn c = get_alias();
 stack pointer_fn d = get_pointer();
 stack pointer_alias_fn e = get_pointer_alias();
 if a(values) != 66 { ret 1; }
 if b(values) != 66 { ret 2; }
 if c(values) != 66 { ret 3; }
 if d(values) != 66 { ret 4; }
 if direct_array(values) != 66 { ret 5; }
 if direct_alias(values) != 66 { ret 6; }
 if e(values) != 66 { ret 7; }
 if f(values) != 66 { ret 8; }
 if g(values) != 66 { ret 9; }
 ret 0;
}
'''
NEGATIVE = {
 'multidimensional': 'typedef int (*bad_fn)(int values[2][3]);',
 'multidimensional_alias': 'typedef int row[3]; typedef int (*bad_fn)(row values[2]);',
 'bare_function': 'typedef int (*bad_fn)(int callback(int));',
 'bare_unprototyped_function': 'typedef int (*bad_fn)(int callback());',
 'comma_function_typedef': 'typedef int scalar, function_type(int); typedef int (*bad_fn)(function_type callback);',
 'function_typedef': 'typedef int function_type(int); typedef int (*bad_fn)(function_type callback);',
 'complex_bound': 'typedef int (*bad_fn)(int values[static 3]);',
 'direct_multidimensional': 'int bad(int values[2][3]);',
 'direct_bare_function': 'int bad(int callback(int));',
}
def main():
 ap=argparse.ArgumentParser(); ap.add_argument('compiler',type=Path); ap.add_argument('--cc',default='clang'); ap.add_argument('--out',type=Path); args=ap.parse_args()
 compiler=args.compiler.resolve(); out=(args.out or Path(tempfile.mkdtemp(prefix='stasha-callback-parameters-'))).resolve(); out.mkdir(parents=True,exist_ok=True)
 report={'compiler':str(compiler),'sha256':hashlib.sha256(compiler.read_bytes()).hexdigest(),'runs':[],'rejections':[]}
 def run(cmd,name,env=None,good=True,diagnostic=None):
  p=subprocess.run(list(map(str,cmd)),cwd=out,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=120)
  (out/(name+'.log')).write_text(p.stdout)
  if good and p.returncode: raise AssertionError(f'{name}: {p.returncode}\n{p.stdout}')
  if not good and (p.returncode==0 or diagnostic not in p.stdout): raise AssertionError(f'{name}: expected {diagnostic!r} rejection, got {p.returncode}\n{p.stdout}')
  return p.stdout
 (out/'parameters.h').write_text(HEADER); (out/'oracle.c').write_text(C); (out/'parameters.sts').write_text(STS.replace('SEARCH',str(out)))
 report['c_compiler']=run([args.cc,'--version'],'cc-version')
 for opt in (0,1,2,3):
  obj=out/f'parameters-O{opt}.o'; exe=out/f'parameters-O{opt}'
  run([compiler,'obj',out/'parameters.sts',f'-o={opt}','-o',obj],f'compile-O{opt}',dict(os.environ,STS_DUMP_IR_PRE=str(out/f'pre-O{opt}.ll')))
  run([args.cc,f'-O{opt}',out/'oracle.c',obj,'-o',exe],f'link-O{opt}')
  text=run([exe],f'run-O{opt}'); print(f'O{opt}: {text}',end=''); report['runs'].append({'opt':opt,'output':text})
 for name,header in NEGATIVE.items():
  (out/f'{name}.h').write_text(header+'\n')
  src=out/f'{name}.sts'; obj=out/f'{name}.o'; src.write_text(f'@[[freestanding, crash: disable]];\ncheader "{name}.h" search "{out}";\n')
  run([compiler,'obj',src,'-o',obj],name,good=False,diagnostic='unsupported parameter declarator')
  if obj.exists(): raise AssertionError(f'{name}: unexpected object produced')
  report['rejections'].append(name)
 (out/'report.json').write_text(json.dumps(report,indent=2)+'\n'); print(f'{len(NEGATIVE)} unsupported parameter shapes rejected; artifacts: {out}')
if __name__=='__main__': main()
