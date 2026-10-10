#!/usr/bin/env python3
"""Checked I1b.1 call contracts. No negative input ever enters execution."""
import argparse, hashlib, json, tempfile, re
from pathlib import Path
from i1b_cases import matrix
from i1b_gaps import gaps
from run_i0 import Runner, inventory_issues, positive_issues, negative_issues

def run(compiler,artifacts):
    artifacts.mkdir(parents=True,exist_ok=True);work=Path(tempfile.mkdtemp(prefix='i1b-',dir=artifacts));r=Runner(compiler,work)
    before=hashlib.sha256(Path(r.compiler).read_bytes()).hexdigest()
    declared=matrix();inventory=json.loads(Path(__file__).with_name('i1b_inventory.json').read_text())
    issues=inventory_issues([c.name for c in declared],inventory['cases'])
    for c in declared:
        for level in range(4):
            r.case(f'{c.name}-O{level}',c.source,pattern=None if c.positive else c.pattern,execute=c.positive,flags=[f'-o={level}'],files=c.files)
    characterization=[]
    gap_cases=gaps();issues+=inventory_issues(list(gap_cases),inventory['gaps'])
    for name,source in gap_cases.items():
        directory=work/('gap-'+name);directory.mkdir();path=directory/'input.sts';path.write_text(source);out=directory/'DO-NOT-RUN.ll'
        result=r.process([r.compiler,'ir',path,'-o',out,'-o=0'],directory/'compile')
        check=positive_issues(result,out) if result['category']=='exit' and result['returncode']==0 else negative_issues(result,out,r'interface|source|conversion','input.sts')
        characterization.append(dict(name=name,status='accepted-deferred' if result['returncode']==0 else 'rejected-deferred',issues=check))
    after=hashlib.sha256(Path(r.compiler).read_bytes()).hexdigest()
    if before!=after:issues.append('compiler bytes changed')
    (work/'summary.json').write_text(json.dumps(dict(compiler=str(r.compiler),compiler_sha256=before,inventory_issues=issues,results=r.results,characterization=characterization),indent=2)+'\n')
    for result in r.results:
        if result['issues']:print('FAIL',result['name'],result['issues'])
    for result in characterization:
        if result['issues']:print('FAIL deferred characterization',result['name'],result['issues'])
    print(f"{sum(not x['issues'] for x in r.results)}/{len(r.results)} checks passed; inventory={issues}; evidence={work}")
    return bool(issues or any(x['issues'] for x in r.results+characterization))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--compiler',required=True);p.add_argument('--artifacts',required=True,type=Path);a=p.parse_args();raise SystemExit(run(a.compiler,a.artifacts))
