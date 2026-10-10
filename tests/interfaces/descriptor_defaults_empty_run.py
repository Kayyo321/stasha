#!/usr/bin/env python3
"""Run only independently approved exact canonical-empty sources and strong hook."""
import argparse,json,os,subprocess
from pathlib import Path
from descriptor_defaults_empty_cases import CASES
from descriptor_defaults_precheck import ROOT,digest,check_identity
from run_i0 import positive_issues
EXPECTED={'returncode':77,'stdout':'','stderr':'descriptor-panic reason=7 side-effects=0\n'}
def validate_gate(gate,compiler_hash,hook_hash):
 if gate['compiler_sha256']!=compiler_hash:raise RuntimeError('compiler not approved by exact empty gate')
 if gate['hook_sha256']!=hook_hash:raise RuntimeError('hook not approved by exact empty gate')
 if gate['allowed_optimizations']!=[0,1,2,3] or gate['expected_runtime']!=EXPECTED:raise RuntimeError('runtime oracle/optimization gate drift')
 reviewed={}
 for row in gate['reviewed_ir']:
  name=row['name'];key=row['optimization']
  if key not in (0,3) or key in reviewed.setdefault(name,{}):raise RuntimeError('duplicate/unexpected reviewed IR')
  reviewed[name][key]=row
 if set(reviewed)!=set(CASES) or gate['source_fixture_count']!=len(CASES):raise RuntimeError('empty source inventory drift')
 for name,rows in reviewed.items():
  if set(rows)!={0,3} or rows[0]['source_sha256']!=rows[3]['source_sha256']:raise RuntimeError('incomplete source review')
 return reviewed
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--compiler',type=Path,required=True);p.add_argument('--manifest',type=Path,required=True);p.add_argument('--gate',type=Path,required=True);p.add_argument('--gate-sha256',required=True);p.add_argument('--llvm-build',type=Path,required=True);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args()
 e=a.evidence.resolve();e.mkdir(parents=True,exist_ok=True);c=a.compiler.resolve();manifest=json.loads(a.manifest.read_text());gate_hash=digest(a.gate)
 if gate_hash!=a.gate_sha256:raise RuntimeError('independent review gate hash mismatch')
 gate=json.loads(a.gate.read_text());hook=ROOT/'tests/interfaces/descriptor_defaults_panic_hook.c';reviewed=validate_gate(gate,digest(c),digest(hook));check_identity(c,manifest)
 copied=e/'panic_hook.c';copied.write_bytes(hook.read_bytes());obj=e/'panic_hook.o';cc=a.llvm_build.resolve()/'bin/clang';records=[];failures=[]
 def capture(cmd,stem,env=None,timeout=30):
  try:
   r=subprocess.run(list(map(str,cmd)),cwd=ROOT,capture_output=True,text=True,timeout=timeout,env=env);row={'command':list(map(str,cmd)),'category':'signal' if r.returncode<0 else 'exit','returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
  except subprocess.TimeoutExpired:row={'command':list(map(str,cmd)),'category':'timeout','returncode':None,'stdout':'','stderr':''}
  (e/(stem+'.json')).write_text(json.dumps(row,indent=2)+'\n');return row
 version=capture([cc,'--version'],'cc-version')
 if version['returncode'] or not version['stdout'].startswith('clang version 23.'):raise RuntimeError('pinned C compiler mismatch')
 obj.unlink(missing_ok=True)
 hook_build=capture([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-fno-lto','-c',copied,'-o',obj],'compile-hook')
 if hook_build['returncode'] or hook_build['stdout'] or hook_build['stderr'] or not obj.is_file():raise RuntimeError('strong hook build failed')
 hook_object_hash=digest(obj)
 for name,source in CASES.items():
  d=e/name;d.mkdir(exist_ok=True);s=d/'input.sts';s.write_text(source)
  if digest(s)!=reviewed[name][0]['source_sha256']:raise RuntimeError('unapproved empty source: '+name)
  for opt in range(4):
   artifact=d/f'O{opt}.exe';pre=d/f'O{opt}.pre.ll';post=d/f'O{opt}.post.ll';artifact.write_text('stale artifact sentinel');pre.unlink(missing_ok=True);post.unlink(missing_ok=True)
   check_identity(c,manifest)
   built=capture([c,'build',s,'-o',artifact,f'-o={opt}','-l',obj],name+f'/O{opt}-compile',env={**os.environ,'STS_DUMP_IR_PRE':str(pre),'STS_DUMP_IR':str(post)})
   issues=positive_issues(built,artifact)
   if artifact.exists() and artifact.read_bytes()==b'stale artifact sentinel':issues.append('stale output sentinel survived')
   if not pre.exists() or digest(pre)!=reviewed[name][0]['pre_ir_sha256']:issues.append('pre-IR differs from independent review')
   if opt in (0,3) and (not post.exists() or digest(post)!=reviewed[name][opt]['ir_sha256']):issues.append('post-IR differs from independent review')
   record={'name':name,'category':'defined-empty-trap','optimization':opt,'source_sha256':digest(s),'compile':built,'issues':issues}
   if not issues:
    executable_hash=digest(artifact)
    # Recheck every authorization/identity input immediately before execution.
    check_identity(c,manifest)
    if digest(a.gate)!=gate_hash or digest(hook)!=gate['hook_sha256'] or digest(copied)!=gate['hook_sha256'] or digest(obj)!=hook_object_hash or digest(s)!=reviewed[name][0]['source_sha256'] or digest(artifact)!=executable_hash:raise RuntimeError('empty gate/input changed before execution')
    result=capture([artifact],name+f'/O{opt}-execute',timeout=5);record['execution']=result;record['executable_sha256']=executable_hash
    if result['category']!='exit' or any(result[k]!=v for k,v in EXPECTED.items()):issues.append('exact reason7/side-effect/exit oracle failed')
   if issues:failures.append(name+f':O{opt}: '+', '.join(issues))
   records.append(record);check_identity(c,manifest)
   (e/'receipt.json').write_text(json.dumps({'compiler_sha256':manifest['binary_sha256'],'gate_sha256':gate_hash,'hook_sha256':gate['hook_sha256'],'hook_object_sha256':hook_object_hash,'cc_sha256':digest(cc),'results':records,'failures':failures},indent=2)+'\n')
 print(json.dumps({'cases':len(CASES),'rows':len(records),'executions':sum('execution' in x for x in records),'failures':failures},indent=2));return bool(failures)
if __name__=='__main__':raise SystemExit(main())
