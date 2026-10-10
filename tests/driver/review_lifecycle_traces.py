#!/usr/bin/env python3
"""Actual hosted ELF lifecycle traces at O0–O3; subprocesses are bounded."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/harness'))
from compiler_suites import capture,INTERNAL_FAILURE
CASES={
 'ordered_return':('''@[[crash: disable]];
mod trace;
stack i32 state = 0;
@[[init: after("first")]] "second" { state = 17; print.("init2\\n"); }
@[[init]] "first" { print.("init1\\n"); }
@[[exit: after("exit_first")]] { print.("exit2\\n"); }
@[[exit]] "exit_first" { print.("exit1\\n"); }
fn main(void): i32 { if(state != 17) { ret 9; } print.("main\\n"); ret 0; }
''',0,'init1\ninit2\nmain\nexit1\nexit2\n'),
 'quit_runs_exit':((ROOT/'tests/signals/quit_runs_exit.sts').read_text(),0,'before_quit\nexit_block_ran\n'),
 'quit_from_exit':((ROOT/'tests/signals/quit_from_exit.sts').read_text(),1,'exit_block_ran\n'),
 'quit_in_init':('''@[[crash: disable]];
mod trace;
@[[init]] { print.("init\\n"); quit.(7); }
@[[exit]] { print.("exit\\n"); }
fn main(void): i32 { print.("UNREACHABLE\\n"); ret 0; }
''',7,'init\nexit\n'),
}
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('compiler',type=Path);p.add_argument('--artifacts',type=Path);args=p.parse_args()
 compiler=args.compiler.resolve();work=args.artifacts.resolve() if args.artifacts else Path(tempfile.mkdtemp(prefix='stasha-lifecycle-review-'))
 work.mkdir(parents=True,exist_ok=True)
 if list(work.iterdir()):p.error('artifact directory must be empty')
 results=[]
 for name,(source,exitcode,stdout) in CASES.items():
  for opt in range(4):
   d=work/(name+f'_O{opt}');d.mkdir();src=d/'probe.sts';src.write_text(source);out=d/'program'
   comp=capture([compiler,'build',src,f'-o={opt}','-o',out],ROOT,d/'compile',60,extra_env={'STS_DUMP_IR_PRE':str(d/'pre.ll'),'STS_DUMP_IR':str(d/'post.ll')})
   errs=[];run=None
   if comp['category']!='success' or INTERNAL_FAILURE.search(comp['stderr']+comp['stdout']) or not out.is_file():errs.append('compile failed')
   else:
    run=capture([out],d,d/'run',10)
    if run['returncode']!=exitcode or run['category'] in ('crash','timeout','launch-error'):errs.append(f"expected exit {exitcode}, got {run['category']}/{run['returncode']}")
    if run['stdout']!=stdout:errs.append('stdout trace mismatch')
    if run['stderr']:errs.append('unexpected runtime stderr')
   results.append(dict(name=name,optimization=opt,expected_stdout=stdout,expected_exit=exitcode,compile=comp,run=run,problems=errs))
   print(('FAIL: ' if errs else 'PASS: ')+name+f' O{opt}')
 report=dict(compiler=str(compiler),compiler_sha256=hashlib.sha256(compiler.read_bytes()).hexdigest(),results=results)
 (work/'results.json').write_text(json.dumps(report,indent=2)+'\n');print('Retained evidence: '+str(work))
 return int(any(r['problems'] for r in results))
if __name__=='__main__':raise SystemExit(main())
