#!/usr/bin/env python3
"""POSIX regression: independent compiler outputs from a shared working directory."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
ap=argparse.ArgumentParser();ap.add_argument('--compiler',type=Path,required=True);ap.add_argument('--artifacts',type=Path,required=True);a=ap.parse_args()
compiler=a.compiler.resolve();a.artifacts.mkdir(parents=True,exist_ok=True)
work=Path(tempfile.mkdtemp(prefix='concurrent-',dir=a.artifacts)).resolve()
source=work/'input.sts';source.write_text('mod concurrency; ext fn identity(stack u64 x): u64 { ret x; }\n')
def one(i):
 output=work/f'{i}.o';command=[str(compiler),'obj',str(source),'-o',str(output)]
 try:
  p=subprocess.run(command,cwd=work,text=True,capture_output=True,timeout=30)
  row={'command':command,'exit':p.returncode,'stdout':p.stdout,'stderr':p.stderr,'object_exists':output.is_file()}
 except subprocess.TimeoutExpired:row={'command':command,'timeout':True}
 row['passed']=row.get('exit')==0 and row.get('object_exists') and output.read_bytes()[:4]==b'\x7fELF'
 return row
with ThreadPoolExecutor(max_workers=8) as pool:rows=list(pool.map(one,range(64)))
(work/'results.json').write_text(json.dumps({'compiler_sha256':hashlib.sha256(compiler.read_bytes()).hexdigest(),'cases':rows},indent=2))
passed=sum(r['passed'] for r in rows);print(f'{passed}/64 concurrent shared-directory compiles passed; {work}')
raise SystemExit(passed!=64)
