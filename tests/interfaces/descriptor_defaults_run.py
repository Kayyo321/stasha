#!/usr/bin/env python3
"""Portable same-reviewed-source hosted gate; never approve changed source/IR."""
import argparse,json,os,platform,subprocess,sys
from pathlib import Path
from descriptor_defaults_precheck import ROOT,digest,check_identity
from descriptor_defaults_empty_run import validate_gate

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--compiler',type=Path,required=True);p.add_argument('--artifacts',type=Path,required=True);a=p.parse_args()
 if platform.system()!='Linux' or platform.machine() not in ('x86_64','amd64'):raise RuntimeError('descriptor runtime gate requires reviewed Linux x86-64 target')
 compiler=a.compiler.resolve();out=a.artifacts.resolve();out.mkdir(parents=True,exist_ok=True)
 reviewed_file=Path(__file__).with_name('descriptor_defaults_reviewed.json');reviewed_hash=digest(reviewed_file);reviewed=json.loads(reviewed_file.read_text())
 if reviewed['schema']!=1 or reviewed['target']!='x86_64-unknown-linux-gnu':raise RuntimeError('unsupported review inventory')
 # Source hashes are independently reviewed constants, not learned from this
 # workspace. Only the candidate binary identity is captured per invocation.
 manifest={'binary_sha256':digest(compiler),'source':reviewed['source']};check_identity(compiler,manifest)
 hook=ROOT/'tests/interfaces/descriptor_defaults_panic_hook.c'
 gate={**reviewed,'compiler_sha256':manifest['binary_sha256']};validate_gate(gate,digest(compiler),digest(hook))
 manifest_path=out/'invocation-source.json';manifest_path.write_text(json.dumps(manifest,indent=2)+'\n')
 gate_path=out/'same-source-empty-gate.json';gate_path.write_text(json.dumps(gate,indent=2)+'\n')
 llvm=Path(os.environ.get('LLVM_BUILD',str(ROOT/'build/llvm'))).resolve();rows=[]
 commands=[
  [sys.executable,ROOT/'tests/interfaces/descriptor_defaults_test_runner.py'],
  [sys.executable,ROOT/'tests/interfaces/descriptor_defaults_precheck.py','--compiler',compiler,'--manifest',manifest_path,'--evidence',out/'precheck'],
  [sys.executable,ROOT/'tests/interfaces/descriptor_defaults_test_diagnostics.py','--compiler',compiler,'--manifest',manifest_path,'--evidence',out/'diagnostics'],
  [sys.executable,ROOT/'tests/interfaces/descriptor_defaults_empty_run.py','--compiler',compiler,'--manifest',manifest_path,'--gate',gate_path,'--gate-sha256',digest(gate_path),'--llvm-build',llvm,'--evidence',out/'empty-runtime'],
  [sys.executable,ROOT/'tests/interfaces/descriptor_defaults_test_units.py','--llvm-build',llvm,'--evidence',out/'units']
 ]
 for i,command in enumerate(commands):
  check_identity(compiler,manifest)
  if digest(reviewed_file)!=reviewed_hash:raise RuntimeError('reviewed inventory changed')
  result=subprocess.run(list(map(str,command)),cwd=ROOT,capture_output=True,text=True,timeout=600)
  rows.append({'command':list(map(str,command)),'returncode':result.returncode,'stdout':result.stdout,'stderr':result.stderr})
  (out/'receipt.json').write_text(json.dumps({'binary_sha256':manifest['binary_sha256'],'reviewed_inventory_sha256':reviewed_hash,'compiler_trust':reviewed['compiler_trust'],'stages':rows},indent=2)+'\n')
  print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
  if result.returncode:raise RuntimeError('descriptor stage failed: '+str(i))
 check_identity(compiler,manifest);print('PASS reviewed descriptor defaults/null-dispatch gate');return 0
if __name__=='__main__':raise SystemExit(main())
