#!/usr/bin/env python3
"""Compile exact empty candidates for independent review; never execute them."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
from descriptor_defaults_empty_cases import CASES
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--compiler',type=Path,required=True);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();out=a.evidence.resolve();out.mkdir(parents=True,exist_ok=True);compiler=a.compiler.resolve();rows=[]
h=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
for name,source in CASES.items():
 d=out/name;d.mkdir(exist_ok=True);s=d/'input.sts';s.write_text(source)
 for opt in (0,3):
  ir=d/f'DO-NOT-RUN.O{opt}.ll';pre=d/f'DO-NOT-RUN.O{opt}.pre.ll';cmd=[str(compiler),'ir',str(s),'-o',str(ir),f'-o={opt}'];r=subprocess.run(cmd,capture_output=True,text=True,timeout=30,env={**os.environ,'STS_DUMP_IR_PRE':str(pre)});(d/f'O{opt}.stderr').write_text(r.stderr);(d/f'O{opt}.stdout').write_text(r.stdout)
  rows.append({'name':name,'category':'defined-empty-pending-independent-review','optimization':opt,'source_sha256':h(s),'command':cmd,'returncode':r.returncode,'ir_sha256':h(ir) if ir.exists() else None,'pre_ir_sha256':h(pre) if pre.exists() else None})
(out/'receipt.json').write_text(json.dumps({'compiler_sha256':h(compiler),'execution_count':0,'results':rows},indent=2)+'\n')
failed=[r for r in rows if r['returncode']];print(json.dumps(failed,indent=2));print('rows',len(rows),'executions 0');raise SystemExit(bool(failed))
