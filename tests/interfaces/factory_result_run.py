#!/usr/bin/env python3
"""Strict leaf-result runner. Gate A is IR-only; Gate B executes valid cases only."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tempfile
from factory_result_cases import matrix, inventory, LEVELS
from run_i0 import Runner, inventory_issues, ROOT

def load_inventory():
    return json.loads(Path(__file__).with_name('factory_result_inventory.json').read_text())

def schema_issues(cases):
    issues=[]
    if not cases:return ['empty fixture inventory']
    for c in cases:
        prefix=str(c.name)+': '
        if not isinstance(c.name,str) or not re.fullmatch(r'[a-z0-9][a-z0-9-]*',c.name):issues.append(prefix+'invalid name')
        if not isinstance(c.source,str) or not c.source.strip():issues.append(prefix+'missing source')
        if c.category not in ('valid','reject','deferred'):issues.append(prefix+'invalid category')
        if not isinstance(c.boundary,str) or not c.boundary:issues.append(prefix+'missing boundary')
        if not isinstance(c.files,dict) or any(not isinstance(k,str) or not k or Path(k).is_absolute() or '..' in Path(k).parts or k=='input.sts' or not isinstance(v,str) for k,v in c.files.items()):issues.append(prefix+'unsafe support files')
        if c.diagnostic_file not in ('input.sts',*c.files):issues.append(prefix+'unowned diagnostic file')
        if c.category=='valid':
            if type(c.expected_exit) is not int or c.expected_exit!=0 or c.expected_stdout!='' or c.expected_stderr!='':issues.append(prefix+'missing exact runtime oracle')
        elif any(x is not None for x in (c.expected_exit,c.expected_stdout,c.expected_stderr)):issues.append(prefix+'compile-only runtime oracle prohibited')
        for phase,expected in [('a',c.category!='deferred'),('b',c.category=='reject')]:
            diagnostic=getattr(c,'diagnostic_'+phase);span=getattr(c,'span_'+phase)
            if expected:
                if not isinstance(diagnostic,str) or not diagnostic:issues.append(prefix+'missing exact diagnostic '+phase)
                if not isinstance(span,tuple) or len(span)!=2 or any(type(x) is not int or x<1 for x in span):issues.append(prefix+'missing owned span '+phase)
            elif diagnostic is not None or span is not None:issues.append(prefix+'unexpected diagnostic oracle '+phase)
    return issues

def native_issues(case,text,level):
    # Higher optimization may inline the verified aggregate call. Native shape
    # is asserted before optimization at O0; every level still verifies as IR.
    if level:return []
    issues=[];name='factorymod__make' if case.name=='source-import-owned-wrapper' else 'make'
    names=[name]+(['other'] if case.name=='two-factories' else [])
    for factory in names:
        definition=re.search(r'(?ms)^define internal (%[^ ]+) @'+re.escape(factory)+r'\([^\n]*\)[^\n]*\{\n(.*?)^}',text)
        if not definition:
            issues.append('missing exact internal aggregate factory definition '+factory);continue
        root,body=definition.groups();returns=re.findall(r'(?m)^\s*ret '+re.escape(root)+r' ([^\n]+)$',body)
        if len(returns)!=1 or not re.fullmatch(r'%[A-Za-z0-9_.$]+',returns[0]):
            issues.append('factory lacks exactly one raw aggregate SSA return '+factory);continue
        if not re.search(r'(?m)^\s*'+re.escape(returns[0])+r' = load '+re.escape(root)+r', ptr ',body):
            issues.append('factory return is not its original whole-root load '+factory)
        calls=list(re.finditer(r'(?m)^\s*(%[A-Za-z0-9_.$]+) = call '+re.escape(root)+r' @'+re.escape(factory)+r'\([^\n]*\)',text))
        if not calls:issues.append('missing direct aggregate result call '+factory)
        for call in calls:
            # Local initialization copies the returned aggregate into a new
            # addressable slot, rather than reusing callee storage.
            following=text[call.end():].split('\n}',1)[0]
            if not re.search(r'(?m)^\s*store '+re.escape(root)+r' '+re.escape(call[1])+r', ptr %[A-Za-z0-9_.$]+',following):
                issues.append('aggregate result has no explicit caller storage copy '+factory)
    if case.name=='argument-order-once':
        first=list(re.finditer(r'call i32 @tick\(i32 1\)',text));second=list(re.finditer(r'call i32 @tick\(i32 7\)',text))
        factory=list(re.finditer(r'call %H @make\(',text))
        if len(first)!=1 or len(second)!=1 or len(factory)!=1 or not first[0].start()<second[0].start()<factory[0].start():
            issues.append('primitive actual/factory calls are not left-to-right once-only at O0')
    return issues

def run(compiler,artifacts,phase='gate-a'):
    artifacts.mkdir(parents=True,exist_ok=True);work=Path(tempfile.mkdtemp(prefix='factory-result-'+phase+'-',dir=artifacts))
    try:
        cases=matrix();issues=schema_issues(cases)
        declared=load_inventory()
        if not isinstance(declared,list) or any(not isinstance(row,dict) or not isinstance(row.get('name'),str) for row in declared):issues.append('invalid frozen inventory schema')
        elif not issues:
            issues+=inventory_issues([c.name for c in cases],[r['name'] for r in declared])
            if declared!=json.loads(json.dumps(inventory(cases))):issues.append('source/category/phase/execution/diagnostic/runtime inventory drift')
    except (OSError,ValueError,TypeError,AttributeError) as error:
        cases=[];issues=['invalid fixture inventory: '+str(error)]
    if phase not in ('gate-a','gate-b'):issues.append('unknown qualification phase')
    if issues:
        (work/'summary.json').write_text(json.dumps(dict(inventory_issues=issues,results=[]),indent=2)+'\n')
        print('FAIL before compiler execution:',issues);return True
    runner=Runner(compiler,work);before=hashlib.sha256(Path(runner.compiler).read_bytes()).hexdigest()
    for c in cases:
        diagnostic=c.diagnostic_a if phase=='gate-a' else c.diagnostic_b
        span=c.span_a if phase=='gate-a' else c.span_b
        execute=phase=='gate-b' and c.category=='valid'
        for level in LEVELS:
            name=f'{c.name}-O{level}'
            runner.case(name,c.source,pattern=re.escape(diagnostic) if diagnostic else None,execute=execute,
                flags=[f'-o={level}'],files=c.files,locfile=c.diagnostic_file,stale=bool(diagnostic))
            result=runner.results[-1];result.update(category=c.category,execute=execute,phase=phase,boundary=c.boundary)
            if diagnostic:
                stderr=(work/name/'compile.stderr').read_text()
                loc=re.search(re.escape('error: '+diagnostic)+r'\n\s*--> (.*?):(\d+):(\d+)\n',stderr)
                reported=Path(loc[1]) if loc else None
                if reported is not None and not reported.is_absolute():reported=ROOT/reported
                owned=(work/name/c.diagnostic_file).resolve()
                if not loc or reported.resolve()!=owned or (int(loc[2]),int(loc[3]))!=span:
                    result['issues'].append('exact owned diagnostic file/span differs')
            if not execute and (work/name/'execute.json').exists():result['issues'].append('compile-only execution prohibition violated')
            if execute:
                native_name=name+'-native'
                output=runner.case(native_name,c.source,execute=False,flags=[f'-o={level}'],files=c.files)
                native=runner.results[-1];native.update(category='native-ir',execute=False,phase=phase,boundary=c.boundary)
                if not native['issues']:native['issues']+=native_issues(c,output.read_text(),level)
                if (work/native_name/'execute.json').exists():native['issues'].append('IR fixture execution prohibition violated')
    if before!=hashlib.sha256(Path(runner.compiler).read_bytes()).hexdigest():issues.append('compiler changed during qualification')
    (work/'summary.json').write_text(json.dumps(dict(compiler=runner.compiler,compiler_sha256=before,phase=phase,
        inventory_issues=issues,results=runner.results),indent=2)+'\n')
    for result in runner.results:
        if result['issues']:print('FAIL',result['name'],result['issues'])
    print(f'{sum(not r["issues"] for r in runner.results)}/{len(runner.results)} checks; evidence={work}')
    return bool(issues or any(r['issues'] for r in runner.results))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--compiler',required=True);p.add_argument('--artifacts',required=True,type=Path)
    p.add_argument('--phase',choices=('gate-a','gate-b'),default='gate-a');a=p.parse_args();raise SystemExit(run(a.compiler,a.artifacts,a.phase))
