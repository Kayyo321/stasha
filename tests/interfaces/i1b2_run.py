#!/usr/bin/env python3
"""Exact local-transfer checks; every negative is compile-only and never run."""
import argparse,hashlib,json,tempfile
from pathlib import Path
from i1b2_cases import matrix
from run_i0 import Runner,inventory_issues

def run(compiler,artifacts):
    artifacts.mkdir(parents=True,exist_ok=True);work=Path(tempfile.mkdtemp(prefix='i1b2-',dir=artifacts));r=Runner(compiler,work)
    before=hashlib.sha256(Path(r.compiler).read_bytes()).hexdigest();declared=matrix()
    inventory=json.loads(Path(__file__).with_name('i1b2_inventory.json').read_text());issues=inventory_issues([c.name for c in declared],inventory['cases'])
    for c in declared:
        for level in range(4):r.case(f'{c.name}-O{level}',c.source,pattern=None if c.positive else c.pattern,execute=c.positive,flags=[f'-o={level}'])
    if before!=hashlib.sha256(Path(r.compiler).read_bytes()).hexdigest():issues.append('compiler changed')
    (work/'summary.json').write_text(json.dumps(dict(compiler=str(r.compiler),compiler_sha256=before,inventory_issues=issues,results=r.results),indent=2)+'\n')
    for result in r.results:
        if result['issues']:print('FAIL',result['name'],result['issues'])
    print(f"{sum(not x['issues'] for x in r.results)}/{len(r.results)} checks passed; inventory={issues}; evidence={work}")
    return bool(issues or any(x['issues'] for x in r.results))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--compiler',required=True);p.add_argument('--artifacts',required=True,type=Path);a=p.parse_args();raise SystemExit(run(a.compiler,a.artifacts))
