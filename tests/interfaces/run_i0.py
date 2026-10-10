#!/usr/bin/env python3
"""I0 graph/metadata regression runner. Invalid inputs compile only, never run.

Run with the exact candidate compiler and immutable B1 compiler for the real
ABI5 archive mismatch fixture. The historical I0 graph/metadata corpus is retained with explicit current-I1a
oracles. Object lifetimes and complete source-flow/boxing closure remain outside
this stage; the separate I1a runner qualifies declaration/table/native guards.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
BAD_DIAGNOSTIC=r'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:|LLVM (?:IR verification|ERROR)|internal (?:error|compiler)|fatal:|assertion.*failed|segmentation fault|warning:|\(!!\)|\b\d+ warnings? emitted\b|Memory leak detected|undefined reference|linker command failed'
NORMAL_LOG=re.compile(r'^\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\] (?:generating code(?: \(.*\))?|linking|archiving|compiled .*|importing .*|optimization is automatically set to 0.*)$')
def positive_issues(result,output):
    issues=[]
    if result['category']!='exit' or result['returncode']!=0: issues.append('positive compilation failed')
    if output.is_symlink() or not output.is_file(): issues.append('missing/nonregular artifact')
    if result['stdout'] or any(line and not NORMAL_LOG.fullmatch(line) for line in result['stderr'].splitlines()):
        issues.append('unexpected compiler output')
    if re.search(BAD_DIAGNOSTIC,result['stderr'],re.I): issues.append('unexpected diagnostic')
    return issues


def negative_issues(result, output, pattern, filename):
    issues=[]
    if result['stdout']:issues.append('unexpected compiler stdout for negative')
    if result['category'] != 'exit' or result['returncode'] != 1:
        issues.append('rejection requires normal compiler exit 1')
    if output.exists() or output.is_symlink(): issues.append('failed/stale artifact survived')
    if not re.search(pattern,result['stderr'],re.I): issues.append('missing expected diagnostic: '+pattern)
    if not re.search(re.escape(filename)+r':\d+:\d+',result['stderr']): issues.append('missing relevant source location')
    if re.search(BAD_DIAGNOSTIC,result['stderr'],re.I):
        issues.append('internal/fatal/warning output is not semantic rejection')
    if not re.search(r'(?m)^error: ',result['stderr']): issues.append('missing error diagnostic')
    if re.search(r'(?m)^\[[^\]]+\] (?:linking|archiving)(?:\s|$)',result['stderr']):
        issues.append('negative reached linking/archive stage')
    return issues

def inventory_issues(actual,expected):
    issues=[]
    if not expected or not actual:issues.append('empty inventory cannot pass')
    if len(expected)!=len(set(expected)):issues.append('duplicate expected inventory entries')
    if len(actual)!=len(set(actual)):issues.append('duplicate executed inventory entries')
    missing=sorted(set(expected)-set(actual));extra=sorted(set(actual)-set(expected))
    if missing:issues.append('missing cases: '+', '.join(missing))
    if extra:issues.append('unexpected cases: '+', '.join(extra))
    return issues

class Runner:
    def __init__(self,compiler,work,timeout=30):
        self.compiler=str(Path(compiler).resolve());self.work=work;self.timeout=timeout;self.results=[]
    def process(self,command,stem):
        try:
            r=subprocess.run([str(x) for x in command],cwd=ROOT,capture_output=True,text=True,errors='replace',timeout=self.timeout)
            result={'command':[str(x) for x in command],'category':'signal' if r.returncode<0 else 'exit',
                    'returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
        except subprocess.TimeoutExpired as error:
            result={'command':[str(x) for x in command],'category':'timeout','returncode':None,
                    'stdout':str(error.stdout or ''),'stderr':str(error.stderr or '')}
        except OSError as error:
            result={'command':[str(x) for x in command],'category':'launch-error','returncode':None,
                    'stdout':'','stderr':str(error)}
        stem.with_suffix('.json').write_text(json.dumps(result,indent=2)+'\n')
        stem.with_suffix('.stdout').write_text(result['stdout']);stem.with_suffix('.stderr').write_text(result['stderr'])
        return result
    def case(self,name,source,pattern=None,execute=False,flags=(),files=None,mode=None,stale=False,locfile='input.sts'):
        case=self.work/name;case.mkdir();path=case/'input.sts';path.write_text(source)
        for file,text in (files or {}).items(): (case/file).write_text(text)
        mode=mode or ('build' if execute else 'ir')
        output=case/('result' if mode=='build' else 'result.ll' if mode=='ir' else 'result.o')
        if stale: output.write_text('stale result')
        result=self.process([self.compiler,mode,path,'-o',output,*flags],case/'compile')
        # This branch is unconditional for negatives: acceptance cannot reach run.
        if pattern is not None:
            issues=negative_issues(result,output,pattern,locfile)
        else:
            issues=positive_issues(result,output)
            if execute and not issues:
                ran=self.process([output],case/'execute')
                if ran['category']!='exit' or ran['returncode']!=0 or ran['stdout'] or ran['stderr']:
                    issues.append('positive exact exit/output oracle failed')
        self.results.append({'name':name,'source':str(path),'negative':pattern is not None,'issues':issues})
        return output

def methods(n,start=0): return ''.join(f'm{i}(void): i32; ' for i in range(start,start+n))
def implementation(ifaces,n=1):
    return 'type S: struct.['+','.join(ifaces)+'] { ext i32 x; '+''.join(
        f'ext fn m{i}(void): i32 {{ ret this.x + {i}; }} ' for i in range(n))+'};'
def main_for(iface,n=1):
    return f'fn call(stack {iface} v): i32 {{ ret '+ ' + '.join(f'v.m{i}()' for i in range(n))+'; } '+f'fn main(void): i32 {{ stack S s = .{{ .x = 1 }}; ret call(s) - {n*(n+1)//2}; }}'
def depth_source(n,reverse=False):
    decls=['type I0: interface { m0(void): i32; };']+[f'type I{i}: interface.[I{i-1}] {{}};' for i in range(1,n)]
    return 'mod depths; '+''.join(reversed(decls) if reverse else decls)
def diamond_source():
    return ('mod diamond; type A: interface { m0(void): i32; };'
            'type B: interface.[A] { m1(void): i32; };'
            'type C: interface.[A] { m2(void): i32; };'
            'type D: interface.[B,C] { m3(void): i32; };'+implementation(['D'],4)+main_for('D',4))

def run_cases(r):
    plain='mod plain; fn main(void): i32 { ret 0; }'
    for level in range(4):
        r.case(f'diamond-O{level}',diamond_source(),execute=True,flags=[f'-o={level}'])
        r.case(f'generic-control-O{level}','mod generic; fn @comptime[T] id(stack T v): T { ret v; } fn main(void): i32 { ret id.[i32](7) - 7; }',execute=True,flags=[f'-o={level}'])
    r.case('diamond-debug',diamond_source(),execute=True,flags=['-g','-o=0'])
    ir=r.case('diamond-slot-ir',diamond_source(),flags=['-o=0'])
    text=ir.read_text() if ir.is_file() else ''
    wanted=r'@__S__D__vtable = internal constant .*\{ ptr @S\.m0, ptr @S\.m1, ptr @S\.m2, ptr @S\.m3 \}'
    r.results.append({'name':'diamond-exact-slot-order','issues':[] if re.search(wanted,text) else ['wrong four-slot table order']})
    r.case('ordinary-return-storage-control','mod returns; ext fn old(void): heap i32 *r; fn main(void): i32 { ret 0; }',execute=True)
    r.case('empty-interface-declaration','mod empty; type Empty: interface {}; fn main(void): i32 { ret 0; }',execute=True)
    r.case('distinct-origins-conflict','mod pending; type A: interface { x(void): i32; }; type B: interface { x(void): f64; }; type C: interface.[A,B] {};',pattern='distinct-origin interface signature conflict')
    for n in [32,33]:
        r.case(f'own-methods-{n}','mod own; type I: interface { '+methods(n)+'};'+(implementation(['I'],n)+main_for('I',n) if n==32 else ''),
               pattern='maximum of 32 own methods' if n==33 else None,execute=n==32)
        r.case(f'flattened-methods-{n}','mod flat; type A: interface {'+methods(16)+'}; type B: interface {'+methods(n-16,16)+'}; type I: interface.[A,B] {};'+(implementation(['I'],n)+main_for('I',n) if n==32 else ''),
               pattern='maximum of 32 unique flattened methods' if n==33 else None,execute=n==32)
    for n in [8,9]:
        ifaces=[f'I{i}' for i in range(n)];decls='mod limits; '+''.join(f'type I{i}: interface {{ m{i}(void): i32; }};' for i in range(n))
        r.case(f'direct-parents-{n}',decls+'type D: interface.['+','.join(ifaces)+'] {};'+(implementation(['D'],n)+main_for('D',n) if n==8 else ''),
               pattern='maximum of 8 interface parents' if n==9 else None,execute=n==8)
        r.case(f'direct-implementations-{n}',decls+implementation(ifaces,n)+(main_for('I0') if n==8 else ''),
               pattern='maximum of 8 implemented interfaces' if n==9 else None,execute=n==8)
    for reverse in [False,True]:
        for n in [64,65]:
            r.case(f'depth-{n}-'+('reverse' if reverse else 'forward'),depth_source(n,reverse)+(implementation([f'I{n-1}'])+main_for(f'I{n-1}') if n==64 else ''),
                   pattern='maximum inheritance path of 64' if n==65 else None,execute=n==64)
    wide='mod wide; type Root: interface { m0(void): i32; };';parents=['Root']
    for layer in range(16):
        names=[f'W{layer}_{i}' for i in range(8)]
        wide+=''.join(f'type {name}: interface.['+','.join(parents)+'] {};' for name in names)
        parents=names
    r.case('wide-memoized-diamond',wide+implementation([parents[0]])+main_for(parents[0]),execute=True)
    negatives={
        'unknown-parent':('type I: interface.[Missing] {};','unknown parent/implementation'),
        'noninterface-parent':('type Plain: struct {}; type I: interface.[Plain] {};','not an interface'),
        'unknown-implementation':('type S: struct.[Missing] {};','unknown parent/implementation'),
        'noninterface-implementation':('type Plain: struct {}; type S: struct.[Plain] {};','not an interface'),
        'duplicate-interface':('type I: interface {}; type I: interface {};','duplicate interface definition'),
        'type-name-collision':('type I: interface {}; type I: struct {};','ambiguous interface/type name'),
        'duplicate-own':('type I: interface { x(void): i32; x(void): i32; };','duplicate own method'),
        'duplicate-parent':('type P: interface {}; type I: interface.[P,P] {};','duplicate parent/implementation'),
        'duplicate-implementation':('type I: interface {}; type S: struct.[I,I] {};','duplicate parent/implementation'),
        'self-cycle':('type I: interface.[I] {};','inheritance cycle'),
        'two-cycle':('type I: interface.[J] {}; type J: interface.[I] {};','inheritance cycle'),
        'long-cycle':('type I: interface.[J] {}; type J: interface.[K] {}; type K: interface.[I] {};','inheritance cycle'),
        'diamond-cycle':('type A: interface.[D] {}; type B: interface.[A] {}; type C: interface.[A] {}; type D: interface.[B,C] {};','inheritance cycle'),
        'generic-interface':('type I: @comptime[T] interface { x(stack T v): T; };','generic interfaces are unsupported'),
        'generic-type-constraint':('type I: interface {}; type Box: @comptime[T.[I]] struct { ext T x; };','generic interface constraints are unsupported'),
        'generic-function-constraint':('type I: interface {}; fn @comptime[T.[I]] id(stack T v): T { ret v; }','generic interface constraints are unsupported'),
        'return-storage':('type I: interface { x(void): heap i32 *r; };','storage-qualified interface returns'),
        'callback-return-storage':('type I: interface { x(stack fn*(void): stack i32 *r cb): i32; };','storage-qualified interface returns'),
        'distinct-origin-call':('type A: interface { m0(void): i32; }; type B: interface { m0(void): f64; }; type I: interface.[A,B] {}; fn f(stack I v): i32 { ret v.m0(); }','distinct-origin interface signature conflict'),
        'empty-table-unsupported':('type I: interface {}; type S: struct.[I] {}; fn consume(stack I v): i32 { ret 0; } fn main(void): i32 { stack S s = .{}; ret consume(s); }','empty-interface table construction'),
        'pointer-level-nine':('type I: interface { p(stack i32 ********* p): i32; };','pointer type exceeds maximum of 8'),
        'callback-arity-33':('type I: interface { p(stack fn*('+','.join(['i32']*33)+'): i32 cb): i32; };','function pointer type exceeds maximum of 32'),
    }
    for name,(source,pattern) in negatives.items():
        r.case(name,'mod rejected; '+source,pattern=pattern)
    r.case('distinct-origin-table','mod origins; type A: interface { m0(void): i32; }; type B: interface { m0(void): i32; }; type I: interface.[A,B] {};'+implementation(['I'])+main_for('I'),execute=True)
    r.case('own-redeclaration-equal','mod redeclaration; type A: interface { m0(void): i32; }; type I: interface.[A] { m0(void): i32; };'+implementation(['I'])+main_for('I'),execute=True)
    r.case('empty-unused-relation','mod empty; type I: interface {}; type S: struct.[I] {};')
    for mode in ['ir','obj','build']:
        r.case('stale-unknown-'+mode,'mod bad; type I: interface.[Missing] {};',pattern='unknown parent/implementation',mode=mode,stale=True)
    r.case('pointer-level-eight','mod ptr; type I: interface { p(stack i32 ******** p): i32; };')
    r.case('callback-arity-32','mod cb; type I: interface { p(stack fn*('+','.join(['i32']*32)+'): i32 cb): i32; };',pattern='callback/closure interface signatures are unsupported')
    for n in [496,497]:
        iface='I'*n
        r.case(f'formatted-table-name-{n+15}',f'mod names; type {iface}: interface {{ m0(void): i32; }};'+implementation([iface])+ (main_for(iface) if n==496 else ''),
               pattern='formatted interface name exceeds maximum of 511' if n==497 else None,execute=n==496)
    # Source-level alias traversal inspects metadata only, with its own bound.
    for n in [256,257]:
        aliases=''.join(f'type A{i}: '+(f'A{i+1}' if i+1<n else 'i32')+';' for i in range(n))
        r.case(f'alias-metadata-depth-{n}','mod aliasdepth; '+aliases+'type I: interface { x(stack A0 p): i32; };',
               pattern='metadata nesting exceeds maximum of 256' if n==257 else None)
    extra_negatives={
      'any-callback-return-storage':('type I: interface { x(stack any.[fn*(void): heap i32 *r] p): i32; };','storage-qualified interface returns'),
      'alias-callback-return-storage':('type CB: fn*(void): heap i32 *r; type I: interface { x(stack CB p): i32; };','storage-qualified interface returns'),
      'alias-forward-return-storage':('type I: interface { x(stack CB p): i32; }; type CB: fn*(void): heap i32 *r;','storage-qualified interface returns'),
      'alias-metadata-cycle':('type A: B; type B: A; type I: interface { x(stack A p): i32; };','alias cycle in interface metadata'),
      'nullable-callback-metadata':('type I: interface { x(stack ?fn*(void): i32 p): i32; };','nullable composite interface types'),
      'nullable-closure-metadata':('type I: interface { x(stack ?closure.[void]: i32 p): i32; };','nullable composite interface types'),
      'nullable-slice-metadata':('type I: interface { x(stack ?[]i32 p): i32; };','nullable composite interface types'),
      'any-name-overflow':('type A: any.['+','.join(['i32']*100)+'];','generic application name exceeds maximum of 511'),
    }
    for name,(source,pattern) in extra_negatives.items():r.case(name,'mod rejected; '+source,pattern=pattern)
    r.case('nullable-storage-order-control','mod ordinary; ext fn old(void): ?heap i32 *r;')
    r.case('callback-alias-unsupported','mod ordinary; type CB: fn*(void): i32; type I: interface { x(stack CB p): i32; };',pattern='callback/closure interface signatures are unsupported')
    r.case('ordinary-callback-alias-control','mod ordinary; type CB: fn*(void): i32; ext fn f(stack CB p): i32;')
    for n in [511,512]:
        r.case(f'interface-type-name-{n}','mod names; type '+('I'*n)+': interface {};',pattern='formatted type name exceeds maximum of 511' if n==512 else None)
    for order in ['ab','ba']:
        files={'a.sts':'mod a; ext type A: interface { m0(void): i32; };',
               'b.sts':'mod b; ext type B: interface.[A] { m1(void): i32; };'}
        r.case('imports-order-'+order,'mod root; '+''.join(f'imp {x};' for x in order)+implementation(['B'],2)+main_for('B',2),execute=True,files=files)
    r.case('imported-diagnostic','mod root; imp bad;',pattern='unknown parent/implementation',
           files={'bad.sts':'mod bad; ext type I: interface.[Missing] {};'},locfile='bad.sts')
    r.case('imported-collision','mod root; imp a; imp b;',pattern='duplicate interface definition',
           files={'a.sts':'mod a; ext type I: interface {};','b.sts':'mod b; ext type I: interface {};'},locfile='b.sts')
    for n in [64,65]:
        # The imported ancestor cache is completed before the root child.
        ancestor=depth_source(n-1).replace('mod depths;','mod ancestor;').replace('type ','ext type ')
        r.case(f'imported-depth-{n}',f'mod root; imp ancestor; type I{n-1}: interface.[I{n-2}] {{}};',
               pattern='maximum inheritance path of 64' if n==65 else None,files={'ancestor.sts':ancestor},locfile='ancestor.sts' if n==65 else 'input.sts')
    r.case('target-gated-out','mod gated; @[[if: os == "windows"]] type I: interface.[Missing] {}; fn main(void): i32 { ret 0; }',execute=True)

def archives(r):
    # Both providers genuinely compile interface dispatch through inherited slots.
    source=diamond_source().replace('mod diamond;','mod provider;').replace('type ','ext type ').replace('fn call(','ext fn call(')
    source=source[:source.index('fn main(')]+'ext fn run(void): i32 { stack S s = .{ .x = 1 }; ret call(s); }'
    for abi,compiler in [(10,r.compiler)]:
        case=r.work/f'archive-abi{abi}';case.mkdir();libsrc=case/'provider.sts';libsrc.write_text(source)
        archive=case/'provider.a'
        build=r.process([compiler,'lib',libsrc,'-o',archive,'-o=0'],case/'library')
        issues=positive_issues(build,archive)
        marker=Path(str(archive)+'.stasha-abi')
        if not marker.is_file() or not marker.read_text().startswith(f'STASHA_ABI {abi}\n'): issues.append('wrong genuine provider ABI marker')
        caller=case/'caller.sts';caller.write_text('mod caller; lib "provider" from "provider.a"; imp provider; fn main(void): i32 { ret provider.run() - 10; }')
        output=case/'caller'
        if not issues:
            result=r.process([r.compiler,'build',caller,'-o',output,'-o=0'],case/'caller-compile')
            if positive_issues(result,output): issues.append('ABI10 caller failed')
            else:
                ran=r.process([output],case/'caller-run')
                if ran['category']!='exit' or ran['returncode']!=0 or ran['stdout'] or ran['stderr']: issues.append('ABI10 inherited-slot execution failed')
        r.results.append({'name':f'genuine-interface-archive-abi{abi}','issues':issues})

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--compiler',required=True);p.add_argument('--b1-compiler')
    p.add_argument('--artifacts',type=Path,required=True);a=p.parse_args();a.artifacts.mkdir(parents=True,exist_ok=True)
    work=Path(tempfile.mkdtemp(prefix='i0-',dir=a.artifacts));r=Runner(a.compiler,work)
    before=hashlib.sha256(Path(r.compiler).read_bytes()).hexdigest()
    run_cases(r);archives(r)
    expected=json.loads((ROOT/'tests/interfaces/i0_inventory.json').read_text())
    if a.b1_compiler:
        from test_i0_archives import run as run_archive_controls
        failed=run_archive_controls(r.compiler,a.b1_compiler,work/'genuine-archive-controls')
        r.results.append({'name':'genuine-legacy-interface-archives','issues':['archive control failed'] if failed else []})
        expected.append('genuine-legacy-interface-archives')
    inventory=inventory_issues([x['name'] for x in r.results],expected)
    if inventory:r.results.append({'name':'inventory-contract','issues':inventory})
    after=hashlib.sha256(Path(r.compiler).read_bytes()).hexdigest()
    if before!=after:r.results.append({'name':'frozen-compiler','issues':['compiler changed during run']})
    (work/'inventory.json').write_text(json.dumps([x['name'] for x in r.results],indent=2)+'\n')
    report={'omitted_controls':[] if a.b1_compiler else ['genuine ABI5 interface archive rejection: no --b1-compiler supplied'], 'compiler':r.compiler,'sha256_before':before,'sha256_after':after,'results':r.results}
    (work/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    if not a.b1_compiler: print('EXCLUDED: genuine ABI5 interface archive control (no legacy compiler supplied)')
    failed=[x for x in r.results if x['issues']]
    for x in failed:print('FAIL',x['name'],':','; '.join(x['issues']))
    print(f'{"FAIL" if failed else "PASS"}: {len(r.results)-len(failed)}/{len(r.results)} I0 checks; evidence {work}')
    raise SystemExit(bool(failed))
if __name__=='__main__':main()
