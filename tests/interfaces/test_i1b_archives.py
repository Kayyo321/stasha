#!/usr/bin/env python3
"""Genuine ABI7 semantic rejection and rebuilt ABI9 native source-call controls.

The old provider and any unexpectedly accepted negative are NEVER executed.
Sidecars are genuine producer output; no relabeling is permitted.
"""
import argparse,hashlib,json,re,shutil,tempfile
from pathlib import Path
from run_i0 import Runner,positive_issues,negative_issues
OLD={'provider.a':'f5bd2617fb41a3afe5547842a42059ff220b85e2e0e667e92becfb463e2fc3ea','provider.sts':'14d033e6e554219308b30af3bf2b2cd33bfb6bb9721271bdf5c3a5717f4b61c1','provider.a.stasha-abi':'9afff458a2759a6a47674a3ad1f6907c530a80caa1e21d91efb10243f09df93f'}
PROVIDER='''mod provider;
ext type E: enum { A,B };
ext type I: interface { id(void): i32; };
ext type Work: interface { mix(stack E e, stack u8 n, stack i32 *r p, stack I other): i32; };
ext type S: struct.[I,Work] {
 ext i32 value;
 ext fn id(void): i32 { ret this.value; }
 ext fn mix(stack E e, stack u8 n, stack i32 *r p, stack I other): i32 {
  if(e!=E.B) { ret 1; } ret (i32)n + *p + other.id();
 }
};
ext fn run(stack Work object, stack I other, stack i32 *r p): i32 { ret object.mix(E.B,240,p,other); }
ext fn echo(I value): I { ret value; }
'''
CALLER='''mod caller;
lib "provider" from "provider.a"; imp provider;
stack i32 count=0;
fn side(I value): I { count+=1; ret provider.echo(value); }
fn consume(stack I value): i32 { ret value.id(); }
fn check(stack Work object, stack I value): i32 {
 stack i32 data[2]=.{2,9};
 if(provider.run(object,value,&data[0])!=283) { ret 1; }
 if(consume(side(value))!=41 || count!=1) { ret 2; }
 ret 0;
}
fn main(void): i32 { stack S s=.{.value=41}; ret check(s,s); }
'''
def run(compiler,history,artifacts):
 artifacts.mkdir(parents=True,exist_ok=True);work=Path(tempfile.mkdtemp(prefix='i1b-archives-',dir=artifacts)).resolve();r=Runner(compiler,work)
 old=work/'historical';old.mkdir()
 for name,expected in OLD.items():
  if hashlib.sha256((history/name).read_bytes()).hexdigest()!=expected:raise SystemExit('historical bytes changed: '+name)
  shutil.copy2(history/name,old/name)
 shutil.copy2(history/'consumer.sts',old/'consumer.sts')
 for mode in ['ir','obj','build']:
  out=old/('DO-NOT-RUN-'+mode);result=r.process([r.compiler,mode,old/'consumer.sts','-o',out],old/('reject-'+mode))
  issues=negative_issues(result,out,'incompatible ABI metadata','consumer.sts')
  if 'required ABI 9 for target' not in result['stderr']:issues.append('missing ABI9 requirement')
  if re.search(r'(?m)^\[[^\]]+\] (?:importing|generating code|linking|archiving)',result['stderr']):issues.append('archive reached forbidden later stage')
  r.results.append(dict(name='genuine-ABI7-reject-'+mode,issues=issues))
 out=old/'invalid-provider.ll';result=r.process([r.compiler,'ir',old/'provider.sts','-o',out],old/'source-reject')
 r.results.append(dict(name='bad-provider-source-reject',issues=negative_issues(result,out,'interface source argument.*nominal','provider.sts')))
 for level in range(4):
  d=work/f'native-O{level}';d.mkdir();(d/'provider.sts').write_text(PROVIDER);(d/'caller.sts').write_text(CALLER);opt=f'-o={level}';issues=[]
  archive=d/'provider.a';built=r.process([r.compiler,'lib',d/'provider.sts','-o',archive,opt],d/'provider-build');issues+=positive_issues(built,archive)
  if not Path(str(archive)+'.stasha-abi').is_file() or Path(str(archive)+'.stasha-abi').read_text()!='STASHA_ABI 9\nTARGET x86_64-unknown-linux-gnu\n':issues.append('genuine ABI9 sidecar absent')
  ir=d/'provider.ll'
  if not issues:issues+=positive_issues(r.process([r.compiler,'ir',d/'provider.sts','-o',ir,opt],d/'provider-ir'),ir)
  if not issues and not re.search(r'\bcall\b[^\n]*%[a-zA-Z_][a-zA-Z_0-9.]*\(',ir.read_text()):issues.append('provider-owned indirect path missing')
  out=d/'caller'
  if not issues:issues+=positive_issues(r.process([r.compiler,'build',d/'caller.sts','-o',out,opt],d/'caller-build'),out)
  if not issues:
   ran=r.process([out],d/'execute')
   if ran['category']!='exit' or ran['returncode'] or ran['stdout'] or ran['stderr']:issues.append('native value/mutation/evaluation-count oracle failed')
  r.results.append(dict(name=f'genuine-ABI9-native-O{level}',issues=issues))
 (work/'summary.json').write_text(json.dumps(dict(compiler_sha256=hashlib.sha256(Path(r.compiler).read_bytes()).hexdigest(),historical_hashes=OLD,results=r.results),indent=2)+'\n')
 for c in r.results:print('FAIL' if c['issues'] else 'PASS',c['name'],c['issues'])
 print('Evidence:',work);return any(c['issues'] for c in r.results)
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--compiler',required=True);p.add_argument('--historical-archive-dir',required=True,type=Path);p.add_argument('--artifacts',required=True,type=Path);a=p.parse_args();raise SystemExit(run(a.compiler,a.historical_archive_dir,a.artifacts))
