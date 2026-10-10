#!/usr/bin/env python3
"""Genuine semantic ABI8 rebuild boundary and valid cross-archive dispatch.

Never execute the historical invalid provider or any unexpectedly accepted
negative. Historical archive/source/sidecar bytes are hash checked, never relabeled.
"""
import argparse, hashlib, json, re, shutil, tempfile
from pathlib import Path
from run_i0 import Runner, positive_issues, negative_issues
from i1a_native_cases import sources
OLD_ARCHIVE_SHA='ac803ecaf607010f5f162b6a43d4f6a81ffaf9428b948597ba21d95eb9b0a77c'

def record(r,name,issues):r.results.append({'name':name,'issues':issues})
def good(r,cmd,stem,out):
    result=r.process(cmd,stem);return positive_issues(result,out)
def run(compiler,history,artifacts):
    artifacts.mkdir(parents=True,exist_ok=True);work=Path(tempfile.mkdtemp(prefix='i1a-archives-',dir=artifacts)).resolve();r=Runner(compiler,work)
    original=history/'provider.a';assert hashlib.sha256(original.read_bytes()).hexdigest()==OLD_ARCHIVE_SHA
    assert (history/'provider.a.stasha-abi').read_text()=='STASHA_ABI 6\nTARGET x86_64-unknown-linux-gnu\n'
    old=work/'historical';old.mkdir()
    for name in ['provider.a','provider.a.stasha-abi','provider.sts','consumer.sts']:shutil.copy2(history/name,old/name)
    for mode in ['ir','build']:
        out=old/f'forbidden-{mode}';result=r.process([r.compiler,mode,old/'consumer.sts','-o',out],old/f'reject-{mode}')
        issues=negative_issues(result,out,'incompatible ABI metadata','consumer.sts')
        if 'required ABI 8 for target' not in result['stderr']:issues.append('missing current ABI8 requirement')
        if re.search(r'(?m)^\[[^\]]+\] (?:importing|generating code|linking|archiving)',result['stderr']):issues.append('old archive reached import/codegen/link')
        record(r,'genuine-ABI6-reject-'+mode,issues)
    out=old/'provider-invalid.ll';result=r.process([r.compiler,'ir',old/'provider.sts','-o',out],old/'source-reject')
    record(r,'old-provider-source-reject',negative_issues(result,out,'no declared/transitive','provider.sts'))
    for level in range(4):
        d=work/f'native-O{level}';d.mkdir();provider,caller=sources();(d/'provider.sts').write_text(provider);(d/'caller.sts').write_text(caller);opt=f'-o={level}'
        archive=d/'provider.a';issues=good(r,[r.compiler,'lib',d/'provider.sts','-o',archive,opt],d/'provider-build',archive)
        if not Path(str(archive)+'.stasha-abi').is_file() or Path(str(archive)+'.stasha-abi').read_text()!='STASHA_ABI 8\nTARGET x86_64-unknown-linux-gnu\n':issues.append('provider is not genuine ABI8')
        ir=d/'provider.ll'
        if not issues:issues+=good(r,[r.compiler,'ir',d/'provider.sts','-o',ir,opt],d/'provider-ir',ir)
        if not issues:
            calls=re.findall(r'\b(?:tail )?call\b[^\n]*%[a-zA-Z_][a-zA-Z_0-9.]*\(',ir.read_text())
            if len(calls)<16:issues.append('parameterized provider lost executed indirect ABI calls')
        out=d/'caller'
        if not issues:issues+=good(r,[r.compiler,'build',d/'caller.sts','-o',out,opt],d/'caller-build',out)
        if not issues:
            ran=r.process([out],d/'execute')
            if ran['category']!='exit' or ran['returncode'] or ran['stdout'] or ran['stderr']:issues.append('native cross-archive value oracle failed')
        record(r,f'genuine-ABI8-native-O{level}',issues)
    # Imported internal methods may back provider-owned tables. Only a request
    # to manufacture a new local table needs their local addressability.
    d=work/'internal-provider';d.mkdir()
    (d/'provider.sts').write_text('mod provider; ext type I: interface { m(void): u32; }; ext type S: struct.[I] { ext u32 n; int fn m(void): u32 { ret this.n; } }; fn consume(stack I v): u32 { ret v.m(); } ext fn entry(void): u32 { stack S s = .{ .n = 4000000000 }; ret consume(s); } ext fn reflect(stack I v): I { ret v; }')
    archive=d/'provider.a';issues=good(r,[r.compiler,'lib',d/'provider.sts','-o',archive],d/'provider-build',archive)
    (d/'caller.sts').write_text('mod caller; lib "provider" from "provider.a"; imp provider; type T: struct.[I] { ext fn m(void): u32 { ret 4000000000; } }; fn check(stack I v): i32 { stack I returned = provider.reflect(v); if (returned.m() != 4000000000) { ret 1; } ret 0; } fn main(void): i32 { if (provider.entry() != 4000000000) { ret 2; } stack T value = .{}; ret check(value); }')
    out=d/'caller'
    if not issues:issues+=good(r,[r.compiler,'build',d/'caller.sts','-o',out],d/'caller-build',out)
    if not issues:
        ran=r.process([out],d/'execute')
        if ran['category']!='exit' or ran['returncode'] or ran['stdout'] or ran['stderr']:issues.append('provider-owned internal table/descriptor consumer failed')
    record(r,'imported-internal-provider-owned-table',issues)
    (d/'negative.sts').write_text('mod negative; lib "provider" from "provider.a"; imp provider; fn consume(stack I v): u32 { ret v.m(); } fn main(void): i32 { stack S s = .{ .n = 7 }; consume(s); ret 0; }')
    out=d/'negative.ll';result=r.process([r.compiler,'ir',d/'negative.sts','-o',out],d/'negative-build')
    record(r,'imported-internal-local-table-reject',negative_issues(result,out,'imported internal interface implementation is unavailable for local table','provider.sts'))
    (work/'summary.json').write_text(json.dumps({'compiler_sha256':hashlib.sha256(Path(r.compiler).read_bytes()).hexdigest(),'historical_archive_sha256':OLD_ARCHIVE_SHA,'results':r.results},indent=2)+'\n')
    for case in r.results:print(('FAIL' if case['issues'] else 'PASS'),case['name'],case['issues'])
    print('Evidence:',work);return any(case['issues'] for case in r.results)
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--compiler',required=True);p.add_argument('--historical-archive-dir',required=True,type=Path);p.add_argument('--artifacts',required=True,type=Path);a=p.parse_args();raise SystemExit(run(a.compiler,a.historical_archive_dir.resolve(),a.artifacts))
