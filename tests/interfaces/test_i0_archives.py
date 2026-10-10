#!/usr/bin/env python3
"""Genuine ABI5 rejection and ABI9 cross-archive inherited-slot control.

The pinned legacy compiler creates its archive then fails cleanup with two known
registry leaks. That provider failure is recorded as FAIL, never called a clean
build. The archive is used only after exact-version/output/structure checks.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tempfile
from run_i0 import ROOT,Runner,diamond_source,positive_issues,negative_issues
B1_SHA='c1fb7d98ae03a1d4b783f7b8c0a03b6df22346ac471f46f7190947049ab832dc'
LEGACY_LEAK=re.compile(r'^\(!!\) \[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\] Memory leak detected: (25344|256) bytes at 0x[0-9a-f]+$')
def provider_issues(result,output,abi):
    if abi==9:return positive_issues(result,output)
    issues=[];lines=result['stderr'].splitlines();leaks=[m.group(1) for line in lines if (m:=LEGACY_LEAK.fullmatch(line))]
    if result['category']!='exit' or result['returncode']!=1:issues.append('legacy cleanup requires exact exit1')
    if leaks!=['25344','256'] or lines.count('2 warnings emitted')!=1:issues.append('legacy cleanup profile differs')
    cleaned={**result,'returncode':0,'stderr':'\n'.join(line for line in lines if not LEGACY_LEAK.fullmatch(line) and line!='2 warnings emitted')}
    issues+=positive_issues(cleaned,output)
    return issues

def archive_rejection_issues(result,output):
    issues=negative_issues(result,output,'incompatible ABI metadata','caller.sts')
    if re.search(r'(?m)^\[[^\]]+\] (?:importing|generating code|linking|archiving)(?:\s|$)',result['stderr']):
        issues.append('ABI5 mismatch reached import/codegen/link stage')
    if 'required ABI 9 for target x86_64-unknown-linux-gnu' not in result['stderr']:
        issues.append('current ABI9 requirement not diagnosed')
    return issues

def run(compiler,b1,artifacts):
    artifacts.mkdir(parents=True,exist_ok=True);work=Path(tempfile.mkdtemp(prefix='i0-archives-',dir=artifacts));r=Runner(compiler,work)
    baseline=Path(b1).resolve()
    if hashlib.sha256(baseline.read_bytes()).hexdigest()!=B1_SHA:raise SystemExit('FAIL: legacy compiler SHA differs from frozen B1')
    source=diamond_source().replace('mod diamond;','mod provider;').replace('type ','ext type ').replace('fn call(','ext fn call(')
    source=source[:source.index('fn main(')]
    source+='ext fn run(void): i32 { stack S s = .{ .x = 1 }; ret call(s); }'
    reports=[]
    for abi,cc in [(5,str(baseline)),(9,r.compiler)]:
        case=work/f'abi{abi}';case.mkdir();provider=case/'provider.sts';provider.write_text(source);archive=case/'provider.a'
        built=r.process([cc,'lib',provider,'-o',archive,'-o=0'],case/'provider-build')
        issues=provider_issues(built,archive,abi)
        marker=Path(str(archive)+'.stasha-abi')
        if marker.is_symlink() or not marker.is_file() or marker.read_text()!='STASHA_ABI '+str(abi)+'\nTARGET x86_64-unknown-linux-gnu\n':
            issues.append('fresh genuine sidecar missing or mismatched')
        if not issues:
            members=r.process([ROOT/'build/llvm/bin/llvm-ar','t',archive],case/'members')
            symbols=r.process([ROOT/'build/llvm/bin/llvm-nm','--defined-only',archive],case/'symbols')
            if members['category']!='exit' or members['returncode'] or len(members['stdout'].splitlines())!=1 or members['stderr']:
                issues.append('invalid archive member inventory')
            if symbols['category']!='exit' or symbols['returncode'] or symbols['stderr'] or not all(
                name in symbols['stdout'] for name in ['__S__D__vtable','provider__call','provider__run',*[f'provider__S__m{i}' for i in range(4)]]):
                issues.append('interface provider symbols missing')
            ir=case/'provider.ll';emitted=r.process([cc,'ir',provider,'-o',ir,'-o=0'],case/'provider-ir')
            issues+=provider_issues(emitted,ir,abi)
            if not issues:
                table=next((line for line in ir.read_text().splitlines() if line.startswith('@__S__D__vtable =')),'')
                actual=re.findall(r'@(?:provider__S__|S\.)m(\d)',table)
                expected=['0','1','0','2','3'] if abi==5 else ['0','1','2','3']
                if actual!=expected:issues.append('genuine legacy/current slot ordering not established: '+repr(actual))
        caller=case/'caller.sts'
        # Local ordinary-call boxing is an already-supported route. The resulting
        # current table crosses the archive boundary and both sides dispatch it.
        caller.write_text('mod caller; lib "provider" from "provider.a"; imp provider; '
            'fn bridge(stack D v): i32 { ret provider.call(v) + v.m2(); } '
            'fn main(void): i32 { stack S s = .{ .x = 1 }; ret bridge(s) - 13; }')
        output=case/'caller'
        if not issues:
            result=r.process([r.compiler,'build',caller,'-o',output,'-o=0'],case/'consumer-build')
            if abi==5:
                issues+=archive_rejection_issues(result,output)
            else:
                issues+=positive_issues(result,output)
                if not issues:
                    ran=r.process([output],case/'consumer-run')
                    if ran['category']!='exit' or ran['returncode'] or ran['stdout'] or ran['stderr']:
                        issues.append('cross-archive inherited dispatch exact oracle failed')
        reports.append({'abi':abi,'provider_build_status':'FAIL: known cleanup-only failure' if abi==5 else ('PASS' if not issues else 'FAIL'),
                        'control':'genuine ABI5 rejection' if abi==5 else 'ABI9 cross-archive inherited dispatch',
                        'issues':issues,'archive_sha256':hashlib.sha256(archive.read_bytes()).hexdigest() if archive.is_file() else None})
    (work/'summary.json').write_text(json.dumps({'compiler_sha256':hashlib.sha256(Path(r.compiler).read_bytes()).hexdigest(),
        'b1_sha256':B1_SHA,'results':reports},indent=2)+'\n')
    for report in reports:print(('FAIL' if report['issues'] else 'PASS')+': '+report['control']+'; '+str(report['issues']))
    print('Legacy provider cleanup: FAIL (exact known historical leaks; deliberately degraded fixture)')
    print('Evidence:',work)
    return bool(any(x['issues'] for x in reports))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--compiler',required=True);p.add_argument('--b1-compiler',required=True);p.add_argument('--artifacts',type=Path,required=True)
    a=p.parse_args();raise SystemExit(run(a.compiler,a.b1_compiler,a.artifacts))
