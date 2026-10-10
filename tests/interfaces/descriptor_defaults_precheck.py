#!/usr/bin/env python3
"""Fail-closed pre-review gate. Only inventoried conventional-valid sources execute."""
import argparse,hashlib,json,re,subprocess,time
from pathlib import Path
from descriptor_defaults_cases import CASES
ROOT=Path(__file__).resolve().parents[2]
CATEGORIES={'valid','empty-pending-review','compile-only','compile-only-deferred','source-unavailable','reject','layout-budget-reject'}
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def diagnostics(stderr):
 return re.findall(r'^(error(?:\[[^\]]+\])?|warning(?:\[[^\]]+\])?): (.*)$',stderr,re.M)
def compile_matches(expected,rc,stdout,stderr,artifact,source):
 if not isinstance(rc,int) or rc<0:return False
 if rc!=expected['returncode'] or stdout!=expected['stdout']:return False
 if [list(x) for x in diagnostics(stderr)]!=expected['diagnostics']:return False
 if artifact.exists()!=expected['artifact']:return False
 if artifact.exists() and artifact.read_bytes()==b'stale artifact sentinel':return False
 if any(kind.startswith('error') and not message.startswith('aborting due to') for kind,message in diagnostics(stderr)):
  if str(source) not in stderr:return False
 return True
def check_identity(compiler,manifest):
 if digest(compiler)!=manifest['binary_sha256']:raise RuntimeError('compiler identity changed')
 for relative,expected in manifest['source'].items():
  if digest(ROOT/relative)!=expected:raise RuntimeError('source identity changed: '+relative)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--compiler',type=Path,required=True);p.add_argument('--evidence',type=Path,required=True);p.add_argument('--manifest',type=Path,required=True);a=p.parse_args()
 c=a.compiler.resolve();e=a.evidence.resolve();e.mkdir(parents=True,exist_ok=True)
 manifest=json.loads(a.manifest.read_text());inventory_path=Path(__file__).with_name('descriptor_defaults_inventory.json');inventory=json.loads(inventory_path.read_text())
 if set(inventory)!=set(CASES):raise RuntimeError('case inventory changed')
 results=[];failures=[];check_identity(c,manifest)
 for name,case in CASES.items():
  expected_case=inventory[name]
  if case['category'] not in CATEGORIES or case['category']!=expected_case['category']:raise RuntimeError('category drift: '+name)
  d=e/name;d.mkdir(exist_ok=True);source=d/'input.sts';source.write_text(case['source']+'\n')
  if digest(source)!=expected_case['source_sha256']:raise RuntimeError('source fixture drift: '+name)
  for opt in ([0,1,2,3] if case['category']=='valid' else [0,3]):
   expected=expected_case['optimizations'][str(opt)];artifact=d/(f'O{opt}.exe' if case['category']=='valid' else f'O{opt}.ll');mode='build' if case['category']=='valid' else 'ir'
   artifact.write_text('stale artifact sentinel');cmd=[str(c),mode,str(source),'-o',str(artifact),f'-o={opt}'];before=time.monotonic()
   try:r=subprocess.run(cmd,capture_output=True,text=True,timeout=30);rc=r.returncode;out=r.stdout;err=r.stderr
   except subprocess.TimeoutExpired:rc='timeout';out='';err='timeout'
   (d/f'O{opt}.stdout').write_text(out);(d/f'O{opt}.stderr').write_text(err)
   row={'name':name,'category':case['category'],'optimization':opt,'command':cmd,'returncode':rc,'source_sha256':digest(source),'seconds':round(time.monotonic()-before,3),'artifact':artifact.exists()}
   ok=compile_matches(expected,rc,out,err,artifact,source)
   if not ok:failures.append(f'{name}:O{opt} compile category/diagnostic/artifact drift ({rc})')
   # Unexpected acceptance or a drifted compile never grants execution.
   if ok and case['category']=='valid':
    check_identity(c,manifest)
    try:rr=subprocess.run([str(artifact)],capture_output=True,text=True,timeout=5);row['runtime']={'returncode':rr.returncode,'stdout':rr.stdout,'stderr':rr.stderr};ok=rr.returncode==0 and not rr.stdout and not rr.stderr
    except subprocess.TimeoutExpired:row['runtime']={'timeout':True};ok=False
    if not ok:failures.append(f'{name}:O{opt} runtime failed')
   if artifact.exists():row['artifact_sha256']=digest(artifact)
   row['pass']=ok;results.append(row);check_identity(c,manifest)
   (e/'receipt.json').write_text(json.dumps({'compiler':str(c),'binary_sha256':manifest['binary_sha256'],'source_manifest_sha256':digest(a.manifest),'inventory_sha256':digest(inventory_path),'policy':'valid execution only; exact immutable source/diagnostic/artifact categories','results':results,'failures':failures},indent=2)+'\n')
 print(json.dumps({'cases':len(CASES),'rows':len(results),'failures':failures},indent=2));return bool(failures)
if __name__=='__main__':raise SystemExit(main())
