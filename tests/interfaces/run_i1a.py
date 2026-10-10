#!/usr/bin/env python3
"""Bounded I1a binding/table ABI qualification. Negatives are never executed.

Source-actual flow, general transfers/boxing routes, generics, initialization,
lifetimes and runtime CFI remain explicitly outside this checkpoint.
"""
import argparse, hashlib, json, re, tempfile
from pathlib import Path
from i1a_binding_cases import matrix
from i1a_origin_cases import matrix as origin_matrix
from run_i0 import Runner, positive_issues, negative_issues, inventory_issues, diamond_source


def declaration(interface_type, implementation_type=None, result='i32', implementation_result=None, prefix='', params=True):
    implementation_type = implementation_type or interface_type
    implementation_result = implementation_result or result
    ip=f'stack {interface_type} value' if params else 'void'
    sp=f'stack {implementation_type} value' if params else 'void'
    return ('mod binding; '+prefix+f'type I: interface {{ m({ip}): {result}; }}; '
            f'type S: struct.[I] {{ ext fn m({sp}): {implementation_result} {{ }} }};')


def scalar_source(typ, value, prelude=''):
    return (f'mod abi; {prelude} type I: interface {{ m(stack {typ} value): {typ}; count(void): i32; }}; '
        f'type S: struct.[I] {{ ext i32 calls; ext fn m(stack {typ} value): {typ} {{ this.calls += 1; ret value; }} '
        'ext fn count(void): i32 { ret this.calls; } }; '
        f'ext fn exercise(stack I object): i32 {{ stack {typ} v = {value}; '
        'if (object.m(v) != v) { ret 1; } if (object.count() != 1) { ret 2; } ret 0; } '
        f'fn main(void): i32 {{ stack S s = .{{ .calls = 0 }}; stack {typ} v = {value}; '
        'if (exercise(s) != 0) { ret 3; } if (s.calls != 0) { ret 4; } '
        'if (s.I.m(v) != v) { ret 5; } if (s.calls != 1) { ret 6; } ret 0; }')


def cases(r):
    # Source-only eligibility controls never execute, even on unexpected accept.
    # I1b.2 checks these same forbidden ordinary-destination transfers earlier.
    # Exact per-case diagnostics change; source fixtures and negative-only execution do not.
    local_transfer_messages = {
        'generic-designated-typed': 'named constructor identity does not match selected concrete destination',
        'generic-call-typed': 'concrete source identity does not match the original local destination',
        'generic-typed-copy': 'concrete source identity does not match the original local destination',
        'generic-lambda-result': 'concrete source identity does not match the original local destination',
        'generic-callback-result': 'concrete source identity does not match the original local destination',
    }
    restored_local_projections = {"unknown-inferred-projection", "unknown-typed-projection"}
    for item in origin_matrix():
        # Original historical fixtures stay byte-identical and compile-only.
        restored = item["name"] in restored_local_projections
        pattern = (None if restored else 'interface local transfer: '+re.escape("'"+local_transfer_messages[item['name']]+"'")
                   if item['name'] in local_transfer_messages else
                   '(?:generic-origin concrete interface use|unresolved/dependent concrete interface origin).*unsupported'
                   if item['negative'] else None)
        r.case('origin-'+item['name'],item['source'],pattern=pattern,files=item['files'])
        if item['negative'] and not restored and item['name'] not in local_transfer_messages:
            diagnostic=(r.work/('origin-'+item['name'])/'compile.stderr').read_text()
            if 'use a directly declared ordinary concrete value or a call with a declared ordinary result' not in diagnostic:
                r.results[-1]['issues'].append('missing actionable interface-origin guidance')
    for item in matrix():
        r.case('binding-'+item['name'],item['source'],pattern=item['diagnostic'])
    values={'bool':'true','i8':'-120','u8':'240','i16':'-30000','u16':'60000','i32':'-2000000000',
            'u32':'4000000000','i64':'-8000000000000000000','u64':'16000000000000000000','f32':'1.25','f64':'3.125'}
    for level in range(4):
        for typ,value in values.items():r.case(f'native-{typ}-O{level}',scalar_source(typ,value),execute=True,flags=[f'-o={level}'])
        r.case(f'native-enum-O{level}',scalar_source('E','E.Last','type E: enum { First, Last };'),execute=True,flags=[f'-o={level}'])
        r.case(f'diamond-O{level}',diamond_source(),execute=True,flags=[f'-o={level}'])
    ptr=('mod pointers; type I: interface { id(stack i32 *rw p): i32 *rw; touch(stack i32 *rw p): void; }; '
         'type S: struct.[I] { ext fn id(stack i32 *rw p): i32 *rw { ret p; } '
         'ext fn touch(stack i32 *rw p): void { p[0] = p[0] + 1; } }; '
         'ext fn check(stack I object, stack i32 *rw p): i32 { object.touch(p); if (object.id(p) != p) { ret 1; } ret 0; } '
         'fn main(void): i32 { stack S s = .{}; stack i32 n = 40; stack i32 *rw p = &n; '
         'if (check(s,p) != 0) { ret 2; } s.I.touch(p); if (s.I.id(p) != p) { ret 3; } ret n - 42; }')
    desc=('mod descriptors; type Base: interface { id(void): u32; }; type Child: interface.[Base] {}; '
          'type Relay: interface { echo(stack Base v): Base; }; '
          'type S: struct.[Child,Relay] { ext u32 n; ext fn id(void): u32 { ret this.n; } '
          'ext fn echo(stack Base v): Base { ret v; } }; '
          'ext fn check(stack Relay relay, stack Base value): i32 { stack Base result = relay.echo(value); '
          'if (result.id() / 2 != 2000000000) { ret 1; } ret 0; } '
          'fn main(void): i32 { stack S s = .{ .n = 4000000000 }; if (check(s,s) != 0) { ret 2; } '
          'ret 0; }')
    for level in range(4):
        r.case(f'native-pointer-void-O{level}',ptr,execute=True,flags=[f'-o={level}'])
        r.case(f'native-descriptor-O{level}',desc,execute=True,flags=[f'-o={level}'])
    # Formal ABI must be in the actual parameterized indirect body.
    ir=r.case('formal-u8-ir',scalar_source('u8','240'),flags=['-o=0'])
    text=ir.read_text() if ir.is_file() else ''
    r.results.append({'name':'formal-u8-ir-proof','issues':[] if re.search(r'call i8 %fn_ptr\(ptr %iface_obj, i8 ',text) and
        re.search(r'@__S__I__vtable = internal constant .*ptr @S.m, ptr @S.count',text) and 'ptr null' not in next((x for x in text.splitlines() if x.startswith('@__S__I__vtable')),'') else ['formal indirect signature/table proof failed']})
    for a,b in [('i32','u32'),('i8','u8'),('bool','i8'),('i32','i64'),('f32','f64')]:
        r.case(f'param-mismatch-{a}-{b}',declaration(a,b),pattern='selected interface implementation signature mismatch')
        r.case(f'return-mismatch-{a}-{b}',declaration('i32',result=a,implementation_result=b),pattern='selected interface implementation signature mismatch')
    for name,prefix,a,b in [
        ('enum-integer','type E: enum { A };','E','i32'),('enum-enum','type E: enum { A }; type F: enum { A };','E','F'),
        ('struct-pointees','type A: struct {}; type B: struct {};','A *rw','B *rw'),
        ('interface-descriptors','type A: interface {}; type B: interface {};','A','B'),
        ('nullable','','?i32 *rw','i32 *rw'),('pointer-depth','','i32 *rw','i32 *rw *rw'),
        ('outer-permission','','i32 *r','i32 *rw'),('inner-permission','','i32 *r *rw','i32 *w *rw'),
        ('arithmetic-permission','','i32 *','i32 *rw')]:
        r.case(name,declaration(a,b,prefix=prefix),pattern='selected interface implementation signature mismatch')
    for name,prefix,typ,pattern in [
        ('aggregate','type A: struct {};','A','by-value struct/union'),('union','type A: union { i32 x; };','A','by-value struct/union'),
        ('payload','type A: enum { X(stack i32) };','A','payload-enum'),('callback','','fn*(stack i32): i32','callback/closure'),
        ('callback-pointer','','fn*(stack i32): i32 *rw','callback/closure'),('closure','','closure.[stack i32]: i32','callback/closure'),
        ('slice','','[]i32','slice interface'),('error','','error','error/handle'),('future','','future','error/handle'),
        ('void-alias','type V: void;','V','void value parameter'),('unresolved','','Missing','unresolved nominal'),
        ('inner-nullable','type P: ?i32 *r;','P *rw','inner-nullable'),
        ('alias-storage','type P: heap i32 *r;','P','storage-qualified aliases')]:
        r.case('unsupported-'+name,declaration(typ,prefix=prefix),pattern=pattern)
    for depth in [1,8]:r.case(f'pointer-depth-{depth}',declaration('i32 '+'*rw '*depth))
    r.case('composed-pointer-alias',declaration('P *w',prefix='type P: i32 *r;'))
    r.case('alias-nominal-pointer',declaration('A *rw',prefix='type S0: struct {}; type A: S0;'))
    r.case('alias-void-pointer',declaration('V *rw',prefix='type V: void;'))
    r.case('nullable-alias-use-syntax-excluded',declaration('?P',prefix='type P: i32 *r;'),pattern='nullable qualifier is only valid on pointer types')
    r.case('nullable-alias-definition',declaration('P',prefix='type P: ?i32 *r;'))
    r.case('enum-alias',declaration('E','A',prefix='type E: enum { X }; type A: E;'))
    for typ in ['u8','f64']:
        chain=''.join(f'type A{i}: '+(f'A{i+1}' if i<69 else typ)+';' for i in range(70))
        r.case(f'long-alias-{typ}',declaration(typ,'A0',result=typ,implementation_result='A0',prefix=chain))
    for level in range(4):
        for typ,value in [('u8','240'),('f64','3.125')]:
            chain=''.join(f'type A{i}: '+(f'A{i+1}' if i<69 else typ)+';' for i in range(70))
            program=scalar_source(typ,value,chain).replace(f'ext fn m(stack {typ} value): {typ}', 'ext fn m(stack A0 value): A0').replace('this.calls += 1; ret value;', 'this.calls += 1; value = value + 1; ret value - 1;')
            r.case(f'long-alias-live-{typ}-O{level}',program,execute=True,flags=[f'-o={level}'])
            start=program.index('fn main(');program=program[:program.index('type I: interface')]+program[start:]+program[program.index('type I: interface'):start]
            r.case(f'long-alias-main-first-{typ}-O{level}',program,execute=True,flags=[f'-o={level}'])
    r.case('missing-unused','mod missing; type I: interface { a(void): i32; b(void): i32; }; type S: struct.[I] { ext fn a(void): i32 { ret 1; } };',pattern='missing instance implementation')
    r.case('empty-unused','mod empty; type I: interface {}; type S: struct.[I] {};')
    r.case('undeclared-boxing','mod undeclared; type I: interface { m(void): i32; }; type S: struct { ext fn m(void): i32 { ret 1; } }; fn f(stack I x): i32 { ret x.m(); } fn main(void): i32 { stack S s = .{}; ret f(s); }',pattern='no declared/transitive')
    r.case('pointer-receiver','mod ptrrecv; type I: interface { m(void): i32; }; fn f(stack I *rw p): i32 { ret p.m(); }',pattern='pointer-to-interface descriptor receiver')
    for name,args,pattern in [('too-few','','argument count'),('too-many','1,2','argument count'),('wrong-representation','"text"','interface source argument.*pointer-versus-value source category mismatch')]:
        r.case('dynamic-'+name,'mod calls; type I: interface { m(stack i32 x): i32; }; fn f(stack I p): i32 { ret p.m('+args+'); }',pattern=pattern)
    r.case('equal-origins','mod origins; type A: interface { m(stack i32 v): u8; }; type U: u8; type B: interface { m(stack i32 v): U; }; type C: interface.[A,B] {}; type S: struct.[C] { ext fn m(stack i32 v): u8 { ret 240; } };')
    r.case('conflicting-origins','mod origins; type A: interface { m(stack i32 v): u8; }; type B: interface { m(stack u32 v): u8; }; type C: interface.[A,B] {};',pattern='distinct-origin interface signature conflict')
    r.case('owner-self','mod self; type I: interface { m(void): i32; }; type S: struct.[I] { ext fn S.m(void): i32 { ret 9; } }; fn f(stack I p): i32 { ret p.m(); } fn main(void): i32 { stack S s = .{}; ret f(s) - 9; }',execute=True)
    r.case('invalid-exact-no-fallback','mod selected; type I: interface { m(stack i32 x): i32; }; type S: struct.[I] { ext fn m(stack i32 x): i32 { ret x; } ext fn I.m(stack u32 x): i32 { ret 0; } };',pattern='selected interface implementation signature mismatch')
    r.case('unknown-unused-qualifier','mod qual; type S: struct { ext fn Missing.m(void): i32 { ret 0; } };',pattern='unknown interface qualifier')
    r.case('unrelated-unused-qualifier','mod qual; type I: interface { m(void): i32; }; type S: struct { ext fn I.m(void): i32 { ret 0; } };',pattern='unrelated interface qualifier')
    # Ordinary callback/generic functions are outside interface preflight.
    r.case('ordinary-callback-generic','mod ordinary; fn apply(stack fn*(stack i32): i32 cb, stack i32 v): i32 { ret cb(v); } fn @comptime[T] id(stack T v): T { ret v; } fn add(stack i32 v): i32 { ret v+1; } fn main(void): i32 { ret apply(add,id.[i32](6))-7; }',execute=True)
    shared='mod candidates; type A: interface { m(void): i32; }; type B: interface.[A] {}; type C: interface.[A] {}; '
    r.case('parent-qualified-supplies-child',shared+'type S: struct.[B] { ext fn A.m(void): i32 { ret 7; } }; fn f(stack B v): i32 { ret v.m(); } fn main(void): i32 { stack S s = .{}; ret f(s)-7; }',execute=True)
    r.case('child-qualified-supplies-parent',shared+'type S: struct.[B] { ext fn B.m(void): i32 { ret 7; } }; fn f(stack A v): i32 { ret v.m(); } fn main(void): i32 { stack S s = .{}; ret f(s)-7; }',execute=True)
    r.case('sibling-qualified-ambiguous',shared+'type S: struct.[B,C] { ext fn B.m(void): i32 { ret 7; } ext fn C.m(void): i32 { ret 9; } };',pattern='ambiguous selected instance implementation')
    r.case('sibling-qualified-parent-resolves',shared+'type S: struct.[B,C] { ext fn A.m(void): i32 { ret 5; } ext fn B.m(void): i32 { ret 7; } ext fn C.m(void): i32 { ret 9; } }; fn f(stack A v): i32 { ret v.m(); } fn main(void): i32 { stack S s = .{}; ret f(s)-5; }',execute=True)
    collision='mod collision; type I: interface { m(void): i32; }; type S: struct.[I] { ext fn m(void): i32 { ret 7; } }; fn S.m(void): i32 { ret 9; } fn f(stack I v): i32 { ret v.m(); } fn main(void): i32 { stack S s = .{}; ret f(s)-7; }'
    for level in range(4):r.case(f'body-declaration-identity-O{level}',collision,execute=True,flags=[f'-o={level}'])
    r.case('qualified-shadows-return-storage','mod shadow; type I: interface { m(void): i32; }; type S: struct.[I] { ext fn m(void): heap i32 *r { ret nil; } ext fn I.m(void): i32 { ret 7; } }; fn f(stack I v): i32 { ret v.m(); } fn main(void): i32 { stack S s = .{}; ret f(s)-7; }',execute=True)
    old_timeout=r.timeout; r.timeout=3
    for flag in ['const','final','atomic','volatile','tls']:
        r.case('parameter-recovery-'+flag,'mod recovery; type I: interface { m(stack '+flag+' i32 *rw p): i32; good(void): i32; }; fn main(void): i32 { ret 0; }',pattern="expected.*found '"+flag+"'")
    r.timeout=old_timeout
    # Repeated DAG has many paths but only a modest number of declaration nodes.
    wide='mod wide; type Root: interface { m(void): i32; }; type Other: interface { m(void): i32; };';parents=['Root']
    for layer in range(12):
        names=[f'W{layer}_{i}' for i in range(4)]
        wide+=''.join(f'type {n}: interface.['+','.join(parents)+'] {};' for n in names);parents=names
    r.case('bounded-repeated-diamond-nonancestor',wide+'type S: struct.['+parents[0]+',Other] { ext fn Root.m(void): i32 { ret 1; } ext fn Other.m(void): i32 { ret 2; } };')


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--compiler',required=True);p.add_argument('--artifacts',type=Path,required=True);p.add_argument('--write-inventory',action='store_true')
    a=p.parse_args();a.artifacts.mkdir(parents=True,exist_ok=True);work=Path(tempfile.mkdtemp(prefix='i1a-',dir=a.artifacts));r=Runner(a.compiler,work)
    compiler_before=hashlib.sha256(Path(r.compiler).read_bytes()).hexdigest()
    cases(r)
    names=[x['name'] for x in r.results];inventory=Path(__file__).with_name('i1a_inventory.json')
    if a.write_inventory:inventory.write_text(json.dumps(names,indent=2)+'\n')
    inventory_errors=inventory_issues(names,json.loads(inventory.read_text()))
    compiler_after=hashlib.sha256(Path(r.compiler).read_bytes()).hexdigest()
    if compiler_before!=compiler_after:inventory_errors.append('compiler changed during qualification')
    summary={'compiler':r.compiler,'compiler_sha256_before':compiler_before,'compiler_sha256_after':compiler_after,'compiler_unchanged':compiler_before==compiler_after,'results':r.results,'inventory_issues':inventory_errors}
    (work/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    for x in r.results:
        if x['issues']:print('FAIL:',x['name'],x['issues'])
    print(f"{len(r.results)-sum(bool(x['issues']) for x in r.results)}/{len(r.results)} cases passed. Evidence: {work}")
    return bool(inventory_errors or any(x['issues'] for x in r.results))
if __name__=='__main__':raise SystemExit(main())
