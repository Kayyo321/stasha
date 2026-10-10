#!/usr/bin/env python3
"""Imported new-diagnostic ownership: compile-only, exact span and stale-output checks."""
import argparse,json,re,os,subprocess
from pathlib import Path
from descriptor_defaults_precheck import ROOT,check_identity,digest
PREFIX='mod provider;ext type I:interface{m(void):i32;};'
CASES={
 'local':PREFIX+'ext fn bad(void):I{stack I value=nil;ret value;}',
 'assignment':PREFIX+'ext fn bad(void):I{stack I value=.{};value=nil;ret value;}',
 'return':PREFIX+'ext fn bad(void):I{ret nil;}',
 'generic':PREFIX+'ext fn @comptime[T] bad(stack T value):I{value;ret nil;}',
 'lambda':PREFIX+'ext fn bad(void):void{stack let _function=lam.(void):I{ret nil;};}',
 'budget':PREFIX+'type D0:struct{ext I value;};'+''.join(f'type D{i}:struct{{ext D{i-1} a;ext D{i-1} b;}};' for i in range(1,14))+'ext fn bad(void):i32{stack D13 _value;ret 0;}'
}
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--compiler',type=Path,required=True);p.add_argument('--manifest',type=Path,required=True);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();compiler=a.compiler.resolve();manifest=json.loads(a.manifest.read_text());e=a.evidence.resolve();e.mkdir(parents=True,exist_ok=True);rows=[];failures=[]
 for name,provider in CASES.items():
  d=e/name;d.mkdir(exist_ok=True);source=d/'provider.sts';source.write_text(provider+'\n')
  if name=='generic':body='stack I _value=bad.[i32](1);ret 0;'
  elif name=='lambda':body='provider.bad();ret 0;'
  elif name=='budget':body='ret provider.bad();'
  else:body='stack I _value=provider.bad();ret 0;'
  caller=d/'caller.sts';caller.write_text('mod caller;imp provider;fn main(void):i32{'+body+'}\n')
  pattern='cannot establish interface default layout: expanded emission budget exhausted' if name=='budget' else 'nil cannot initialize an interface descriptor; use .{} for an empty descriptor or assign an existing interface value'
  column=provider.index('_value')+1 if name=='budget' else provider.index('nil')+1
  for opt in (0,3):
   check_identity(compiler,manifest);output=d/f'forbidden-O{opt}.ll';pre=d/f'forbidden-O{opt}.pre.ll';output.write_text('stale output');cmd=[str(compiler),'ir',str(caller),'-o',str(output),f'-o={opt}'];r=subprocess.run(cmd,capture_output=True,text=True,timeout=30,env={**os.environ,'STS_DUMP_IR_PRE':str(pre)})
   spans=re.findall(re.escape('error: '+pattern)+r'\n\s*--> ([^\n]+)',r.stderr);expected=f'{source}:1:{column}'
   ok=r.returncode==1 and not r.stdout and spans==[expected] and not output.exists() and not pre.exists() and 'warning:' not in r.stderr and 'verification' not in r.stderr.lower()
   row={'name':name,'optimization':opt,'command':cmd,'returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr,'expected_span':expected,'source_sha256':digest(source),'pass':ok};rows.append(row)
   if not ok:failures.append(name+':O'+str(opt))
   (d/f'O{opt}.stderr').write_text(r.stderr)
 # Interleaved imported and primary errors must each retain their literal owner.
 d=e/'restoration';d.mkdir(exist_ok=True);provider='mod provider;ext type I:interface{m(void):i32;};ext fn bad(void):I{ret nil;}';other='mod other;ext fn bad2(void):I{ret nil;}';caller='mod caller;imp provider;imp other;fn main(void):i32{stack I value=nil;ret value.m();}'
 sources={'provider.sts':provider,'other.sts':other,'caller.sts':caller}
 for file,source in sources.items():(d/file).write_text(source+'\n')
 for opt in (0,3):
  output=d/f'forbidden-O{opt}.ll';output.write_text('stale output');check_identity(compiler,manifest);cmd=[str(compiler),'ir',str(d/'caller.sts'),'-o',str(output),f'-o={opt}'];r=subprocess.run(cmd,capture_output=True,text=True,timeout=30)
  spans=re.findall(r'error: nil cannot initialize an interface descriptor;[^\n]*\n\s*--> ([^\n]+)',r.stderr);expected=sorted(f'{d/file}:1:{text.index("nil")+1}' for file,text in sources.items());ok=r.returncode==1 and not r.stdout and sorted(spans)==expected and not output.exists() and 'warning:' not in r.stderr
  rows.append({'name':'restoration','optimization':opt,'command':cmd,'returncode':r.returncode,'stderr':r.stderr,'expected_spans':expected,'pass':ok})
  if not ok:failures.append('restoration:O'+str(opt))
 check_identity(compiler,manifest);(e/'receipt.json').write_text(json.dumps({'compiler_sha256':manifest['binary_sha256'],'results':rows,'failures':failures},indent=2)+'\n');print(json.dumps({'rows':len(rows),'failures':failures},indent=2));return bool(failures)
if __name__=='__main__':raise SystemExit(main())
