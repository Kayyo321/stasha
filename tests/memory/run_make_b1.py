#!/usr/bin/env python3
"""B1 explicit-count constructors with bounded deterministic allocator faults.

Objects are compiled freestanding and linked to a hosted C driver. Link-time
malloc/calloc wrappers observe only the tested object, avoiding runtime startup
allocations. No request above 4096 bytes reaches a real allocator. Rejection
fixtures are never linked or executed, even if the compiler accepts them.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import tempfile
import run_access_contracts as access

runner = access.scalar_runner
DECL = '''ext fn b1_begin(stack i32 reason, stack i32 calls, stack u64 bytes, stack i32 trace, stack i32 fail): void;
ext fn b1_step(stack i32 label, stack i32 value): i32;
ext fn b1_watch(stack void *rw p): void;
ext fn b1_empty(stack void *r p): i32;
ext fn b1_use(stack void *r p): void;
'''
SUPPORT = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct view { void *data; int32_t len, cap; };
static int expected_reason, expected_calls, expected_trace = -1, force_fail;
static int calls, trace;
static uint64_t expected_bytes, actual_bytes;
static const void *watched;
static struct view before;
extern int b1_run(void);
extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
static _Noreturn void fail(const char *why) {
    fprintf(stderr, "B1_HOOK_MISMATCH:%s:reason=%d:calls=%d:bytes=%llu:trace=%d\n",
        why, expected_reason, calls, (unsigned long long)actual_bytes, trace);
    _Exit(87);
}
void b1_begin(int reason, int count, uint64_t bytes, int order, int oom) {
    expected_reason=reason; expected_calls=count; expected_bytes=bytes;
    expected_trace=order; force_fail=oom; calls=trace=0; actual_bytes=0; watched=NULL;
}
int b1_step(int label, int value) { trace=trace*10+label; return value; }
void b1_watch(void *p) { watched=p; memcpy(&before,p,sizeof before); }
int b1_empty(const void *p) {
    struct view v; memcpy(&v,p,sizeof v); return !v.data && !v.len && !v.cap;
}
void b1_use(const void *p) {
    struct view v; memcpy(&v,p,sizeof v);
    if (v.len<0 || v.cap<v.len || (!v.cap && v.data) || (v.cap && !v.data)) fail("invalid observed descriptor");
    if (v.len) { volatile unsigned char first=*(const volatile unsigned char *)v.data; (void)first; }
}
static int request(size_t n) {
    ++calls; actual_bytes=n;
    if (calls > expected_calls || n != expected_bytes) fail("unexpected allocation");
    if (force_fail) return 0;
    if (n > 4096) fail("request exceeds bounded allocator");
    return 1;
}
void *__wrap_malloc(size_t n) {
    if (!request(n)) return NULL;
    void *p=__real_malloc(n); if (!p) fail("host allocation failed");
    memset(p,0xa5,n); return p;
}
void *__wrap_calloc(size_t count, size_t stride) {
    if (stride && count > SIZE_MAX/stride) fail("calloc size overflow");
    if (!request(count*stride)) return NULL;
    void *p=__real_calloc(count,stride); if (!p) fail("host allocation failed"); return p;
}
static void verify(void) {
    if (calls != expected_calls || actual_bytes != expected_bytes) fail("allocation count or bytes");
    if (expected_trace >= 0 && trace != expected_trace) fail("operand evaluation order");
    if (watched && memcmp(watched,&before,sizeof before)) fail("destination published before panic");
}
_Noreturn void __stasha_panic(uint32_t reason) {
    if (reason != (uint32_t)expected_reason || (reason != 6 && reason != 8)) fail("panic reason");
    verify(); fputs(reason==6 ? "STASHA_TRAP:bounds\n" : "STASHA_TRAP:allocation\n",stderr);
    fflush(stderr); _Exit(86);
}
int main(void) {
    int result=b1_run(); if (result) fail("program result");
    if (expected_reason) fail("expected panic missing"); verify(); return 0;
}
'''


def program(name, body, declarations=''):
    return ('@[[freestanding]];\nmod make_' + name + ';\n' + DECL + declarations +
            '\next fn b1_run(void): i32 {\n' + body + '\n}\n')


def cases():
    result = []
    def add(name, body, declarations='', reason=0, diagnostic=''):
        if not diagnostic:
            # External observation keeps the successful allocation live across
            # LLVM's otherwise permitted allocation elision. Count/OOM oracles
            # concern retained allocations, not compiler-folded allocations.
            body=re.sub(r'(heap\s+(?:let|\[\]\w+)\s+(\w+)\s*=\s*(?:make\.\(|construct\.)[^;]+;)',
                r'\1 b1_use(&\2);',body)
        result.append(access.Case(name, program(name, body, declarations),
            outcome='rejection' if diagnostic else 'trap' if reason else 'success',
            reason={6:'bounds',8:'allocation'}.get(reason,''), diagnostic=diagnostic))
    def begin(reason=0, calls=1, size=12, trace=-1, fail=0):
        return f'b1_begin({reason},{calls},{size},{trace},{fail}); '
    sizes={'i8':1,'u8':1,'i16':2,'u16':2,'i32':4,'u32':4,'i64':8,'u64':8,'bool':1,'f32':4,'f64':8}
    for typ,size in sizes.items():
        zero='false' if typ=='bool' else '0.0' if typ in ('f32','f64') else '0'
        add('default_'+typ,begin(size=5*size)+f'heap []{typ} s=make.([]{typ},3,5); '
            f'if len.(s)!=3 || cap.(s)!=5 || s[0]!={zero} || s[2]!={zero} {{ ret 1; }} rem.(s); ret 0;')
    for typ in ('i8','u8','i16','u16','i32','u32','i64','u64','bool'):
        value='true' if typ=='bool' else '1'
        add('count_'+typ,begin(size=4)+f'stack {typ} n={value}; heap []i32 s=make.([]i32,n); '
            'if len.(s)!=1 || cap.(s)!=1 || s[0]!=0 { ret 1; } rem.(s); ret 0;')
    add('count_unsigned_top_bit',begin(size=256)+'stack u8 n=128; heap []i16 s=make.([]i16,n); '
        'if len.(s)!=128 || s[127]!=0 { ret 1; } rem.(s); ret 0;')
    for body,name in [('make.([]i32,0)','zero_omitted'),('make.([]i32,0,0)','zero_explicit'),
                     ('make.([]i32,b1_step(1,0),b1_step(2,0))','zero_order')]:
        add(name,begin(calls=0,size=0,trace=12 if name=='zero_order' else -1)+
            'heap []i32 s='+body+'; if b1_empty(&s)!=1 { ret 1; } rem.(s); ret 0;')
    add('zero_length_positive_capacity',begin(size=12)+'heap []i32 s=make.([]i32,0,3); '
        'if len.(s)!=0 || cap.(s)!=3 || b1_empty(&s)!=0 { ret 1; } rem.(s); ret 0;')
    add('once_explicit',begin(size=16,trace=12)+'heap []i32 s=make.([]i32,b1_step(1,2),b1_step(2,4)); '
        'if len.(s)!=2 || cap.(s)!=4 { ret 1; } rem.(s); ret 0;')
    add('once_omitted',begin(size=8,trace=1)+'heap []i32 s=make.([]i32,b1_step(1,2)); '
        'if len.(s)!=2 || cap.(s)!=2 { ret 1; } rem.(s); ret 0;')
    for typ,value in [('i8','-1'),('i16','-1'),('i32','-1'),('i64','-1'),
                      ('i64','2147483648'),('u32','2147483648'),('u32','4294967295'),
                      ('u64','4294967296'),('u64','4294967297'),('u64','18446744073709551615')]:
        label=typ+'_'+value.replace('-','neg')
        for slot,expression in [('len','make.([]i32,n)'),('cap','make.([]i32,0,n)')]:
            add('invalid_'+slot+'_'+label,begin(6,0,0)+f'stack {typ} n={value}; '
                'heap []i32 _s='+expression+'; ret 0;',reason=6)
    add('len_exceeds_cap',begin(6,0,0)+'heap []i32 _s=make.([]i32,3,2); ret 0;',reason=6)
    add('invalid_count_order',begin(6,0,0,12)+'heap []i32 _s=make.([]i32,b1_step(1,-1),b1_step(2,2)); ret 0;',reason=6)
    add('unsafe_count_checks',begin(6,0,0)+'unsafe { heap []i32 _s=make.([]i32,-1); } ret 0;',reason=6)
    for name,expression,size,trace in [('oom','make.([]i32,2,4)',16,-1),
            ('oom_order','make.([]i32,b1_step(1,2),b1_step(2,4))',16,12),
            ('max_count_admitted','make.([]i64,0,2147483647)',17179869176,-1)]:
        typ='i64' if name=='max_count_admitted' else 'i32'
        add(name,begin(8,1,size,trace,1)+f'heap []{typ} s=nil; b1_watch(&s); s='+expression+'; b1_use(&s); ret 0;',reason=8)
    add('unsafe_oom_checks',begin(8,1,8,-1,1)+'unsafe { heap []i32 _s=make.([]i32,2); } ret 0;',reason=8)
    add('spare_not_readable',begin(6,1,12)+'heap []i32 s=make.([]i32,1,3); stack i32 _x=s[1]; ret 0;',reason=6)
    add('spare_not_resliceable',begin(6,1,12)+'heap []i32 s=make.([]i32,1,3); heap []i32 _v=s[:2]; ret 0;',reason=6)
    add('inferred_make_type',begin()+'heap let s=make.([]i32,3); if s[2]!=0 { ret 1; } rem.(s); ret 0;')
    add('builtin_alias_chain',begin()+'heap []Alias2 s=make.([]Alias,3); if s[2]!=0 { ret 1; } rem.(s); ret 0;',
        'type Alias: i32; type Alias2: Alias;')
    for typ,size in [('u8',1),('f64',8)]:
        zero='0.0' if typ=='f64' else '0'
        add('builtin_alias_stride_'+typ,begin(size=3*size)+f'heap []Alias3 s=make.([]Alias2,3); '
            f'if s[2]!={zero} {{ ret 1; }} rem.(s); ret 0;',
            f'type Alias:{typ}; type Alias2:Alias; type Alias3:Alias2;')
    # Existing return syntax uses a named slice alias (direct []T is parsed as
    # a multi-result list). The alias is resolved while T is instantiated.
    generic='type GenericView:[]T; fn @comptime[T] construct(stack i32 n): GenericView { ret make.([]T,n); }\n'
    add('generic_builtin',begin()+'heap []i32 s=construct.[i32](3); if s[2]!=0 { ret 1; } rem.(s); ret 0;',generic)
    add('generic_alias',begin()+'heap []i32 s=construct.[Alias](3); if s[2]!=0 { ret 1; } rem.(s); ret 0;',
        'type Alias: i32;'+generic)
    rejection='explicit-count make supports only builtin'
    unsupported=[('pointer','P','type P: i32 *rw;'),('pointer_alias','P2','type P: i32 *rw; type P2:P;'),
        ('enum','E','type E: enum { A, B };'),('enum_alias','EA','type E:enum {A,B}; type EA:E;'),
        ('tagged_enum','E','type E:enum {A(stack i32), B};'),
        ('struct','Box','type Box:struct {ext i32 x;};'),('union','Box','type Box:union {i32 x; f32 y;};'),
        ('interface','I','type I:interface { act(void): void; };'),
        ('slice','View','type View:[]i32;'),('void','void',''),('future','future',''),('error','error',''),
        ('fnptr','F','type F:fn*(void):i32;')]
    for name,typ,decl in unsupported:
        add('reject_element_'+name,'heap []'+typ+' _s=make.([]'+typ+',0); ret 0;',decl,diagnostic=rejection)
    add('reject_generic_enum','heap []E _s=construct.[E](0); ret 0;',
        'type E:enum {A,B};'+generic,diagnostic=rejection)
    add('reject_generic_pointer_alias','heap []P _s=construct.[P](0); ret 0;',
        'type P:i32 *rw;'+generic,diagnostic=rejection)
    for name,expr in [('float','1.0'),('pointer','&_n')]:
        add('reject_count_'+name,'stack i32 _n=1; heap []i32 _s=make.([]i32,'+expr+'); ret 0;',
            diagnostic='requires integer length and capacity')
    incompatible='implicit slice conversion requires identical element types'
    for name,body,decl in [
        ('declaration','heap []u32 _s=make.([]i32,0); ret 0;',''),
        ('assignment','heap []u32 s=nil; s=make.([]i32,0); ret 0;',''),
        ('argument','take(make.([]i32,0)); ret 0;','fn take(stack []u32 _s):void {}'),
        ('return','ret 0;','type Result:[]u32; fn wrong(void):Result { ret make.([]i32,0); }'),
        ('conditional_first','heap []u32 other=nil; heap []i32 _s=true ? (make.([]i32,0)) : other; ret 0;',''),
        ('conditional_second','heap []u32 other=nil; heap []u32 _s=true ? other : make.([]i32,0); ret 0;',''),
        ('conditional_context','heap []u32 _s=true ? (make.([]i32,0)) : make.([]i32,0); ret 0;',''),
        ('intermediate','heap []i32 a=make.([]i32,0); heap []u32 _b=a; ret 0;',''),
        ('enum_identity','stack []E a=nil; stack []i32 _b=a; ret 0;','type E:enum {A,B};'),
        ('enum_alias_identity','stack []EA a=nil; stack []i32 _b=a; ret 0;','type E:enum {A,B}; type EA:E;'),
        ('enum_array_identity','stack E a[2]=.{E.A,E.B}; stack []i32 _b=a[:]; ret 0;','type E:enum {A,B};'),
        ('enum_member_identity','stack Box a=.{.values=.{E.A,E.B}}; stack []i32 _b=a.values[:]; ret 0;','type E:enum {A,B}; type Box:struct {ext E values[2];};'),
        ('pointer_layers','stack []P a=nil; stack []Q _b=a; ret 0;','type P:i32 *rw; type Q:i32 *r;'),
        ('pointer_depth','stack []P a=nil; stack []Q _b=a; ret 0;','type P:i32 *rw; type Q:i32 *rw *rw;'),
        ('pointer_nullable','stack []P a=nil; stack []Q _b=a; ret 0;','type P:?i32 *rw; type Q:i32 *rw;'),
        ('pointer_nullable_alias','ret 0;','type I:i32; type NullableView:[]?I *rw; type PlainView:[]I *rw; fn convert(stack NullableView source):PlainView { ret source; }'),
        ('aggregate_identity','stack []A a=nil; stack []B _b=a; ret 0;','type A:struct {ext i32 x;}; type B:struct {ext i32 x;};'),
        ('generic_return','heap []u32 _s=construct.[i32](0); ret 0;',generic),
    ]:
        add('reject_mismatch_'+name,body,decl,diagnostic=incompatible)
    for name,typ,decl in [('pointer','P','type P:i32 *rw; type A:P;'),
                         ('aggregate','P','type P:struct {ext i32 x;}; type A:P;'),
                         ('enum','P','type P:enum {X,Y}; type A:P;')]:
        add('compatible_'+name+'_alias',begin(calls=0,size=0)+f'stack []{typ} s=nil; stack []A t=s; '
            'if len.(t)!=0 { ret 1; } ret 0;',decl)
    add('reject_unresolved_slice_type','stack []Missing a=nil; stack []Missing _b=a; ret 0;',diagnostic='unresolved slice element type')
    add('reject_cyclic_slice_type','stack []A a=nil; stack []A _b=a; ret 0;','type A:[]A;',diagnostic='cyclic slice element type')
    add('reject_alias_cycle_make','heap []A _s=make.([]A,0); ret 0;','type A:B; type B:A;',diagnostic='cyclic or unresolved slice element type')
    add('reject_unknown_make','heap []Missing _s=make.([]Missing,0); ret 0;',diagnostic=rejection)
    add('compatible_enum_array',begin(calls=0,size=0)+'stack E a[2]=.{E.A,E.B}; stack []E s=a[:]; '
        'b1_use(&s); if len.(s)!=2 { ret 1; } ret 0;','type E:enum {A,B};')
    add('compatible_enum_member',begin(calls=0,size=0)+'stack Box a=.{.values=.{E.A,E.B}}; stack []E s=a.values[:]; '
        'b1_use(&s); if len.(s)!=2 { ret 1; } ret 0;','type E:enum {A,B}; type Box:struct {ext E values[2];};')
    return result


def artifact_ok(path, executable=False):
    return path.is_file() and not path.is_symlink() and path.stat().st_size>0 and (not executable or os.access(path,os.X_OK))


def diagnostic_gates(process, allow_error=False, quiet=False):
    text=process['stdout']+'\n'+process['stderr'];problems=[]
    if runner.INTERNAL_FAILURE.search(text):problems.append('internal failure marker')
    if runner.WARNING.search(text):problems.append('unexpected warning')
    if not allow_error and runner.ERROR.search(text):problems.append('unexpected error diagnostic')
    if quiet and (process['stdout'] or process['stderr']):problems.append('unexpected tool output')
    return problems


def rejection_gates(process, source, output, expected):
    problems=diagnostic_gates(process,allow_error=True)
    if process['category']!='exit' or process['returncode']!=1:problems.append('expected diagnostic exit 1')
    if not runner.ERROR.search(process['stderr']):problems.append('missing error diagnostic')
    if not re.search(expected,process['stderr'],re.I):problems.append('missing intended diagnostic')
    if not re.search(re.escape(source.name)+r':[1-9][0-9]*',process['stderr']):problems.append('missing source location')
    if output.exists() or output.is_symlink():problems.append('rejection left output artifact')
    return problems


def success_gates(process, output, executable=False, quiet=False):
    problems=diagnostic_gates(process,quiet=quiet)
    if process['category']!='exit' or process['returncode']!=0:problems.append('tool did not exit successfully')
    if not artifact_ok(output,executable):problems.append('fresh regular output artifact missing')
    return problems


def execute_case(case, opt, args, work, support):
    work.mkdir(parents=True)
    source=work/(case.name+'.sts'); source.write_text(case.source)
    obj=work/'program.o'; binary=work/'program'; pre=work/'pre.ll'; post=work/'post.ll'
    compiled=runner.capture([args.compiler,'obj',source,'-o='+str(opt),'-o',obj],work,work/'compile',30,
        dict(STS_DUMP_IR_PRE=str(pre),STS_DUMP_IR=str(post)))
    problems=[]
    result=dict(name=case.name,optimization=opt,outcome=case.outcome,status='FAIL',problems=problems,
        source=str(source),source_sha256=runner.sha256(source),compile=compiled)
    if case.outcome=='rejection':
        problems+=rejection_gates(compiled,source,obj,case.diagnostic)
        result['status']='FAIL' if problems else 'PASS'
        return result  # Never link or execute a rejection fixture.
    problems+=success_gates(compiled,obj)
    if problems:return result
    linked=runner.capture([args.cc,obj,support,'-Wl,--wrap=malloc,--wrap=calloc','-o',binary],work,work/'link',30)
    result['link']=linked
    problems+=success_gates(linked,binary,executable=True,quiet=True)
    if problems:return result
    executed=runner.capture([binary],work,work/'execute',5);result['run']=executed
    if case.outcome=='trap':
        if executed['category']!='exit' or executed['returncode']!=86: problems.append('wrong panic termination')
        if executed['stdout'] or executed['stderr']!='STASHA_TRAP:'+case.reason+'\n': problems.append('wrong exact panic hook output')
    elif executed['category']!='exit' or executed['returncode']!=0 or executed['stdout'] or executed['stderr']:
        problems.append('runtime oracle failed')
    result['status']='FAIL' if problems else 'PASS';return result


def cross_target_checks(args,work):
    result=[]
    source=work/'target.sts';source.write_text(program('target','heap []i32 _s=make.([]i32,0); ret 0;'))
    for target in ('i386-unknown-none-elf','x86_64-unknown-none-elf'):
        output=work/(target+'.ll')
        proc=runner.capture([args.compiler,'ir',source,'--target',target,'-o',output],work,work/target,30)
        if target.startswith('i386'):
            problems=rejection_gates(proc,source,output,'64-bit target allocator ABI')
        else:problems=success_gates(proc,output)
        result.append(dict(name='target_'+target,status='FAIL' if problems else 'PASS',problems=problems,compile=proc))
    return result


def constructor_ir_problems(text):
    """Audit the unoptimized constructor CFG, not textual block ordering."""
    match=re.search(r'define [^\n]*@b1_ir\([^\n]*\)\s*\{(.*?)^}',text,re.M|re.S)
    if not match:return ['missing constructor function']
    blocks={};current=None
    for line in match.group(1).splitlines():
        label=re.match(r'^([\w.$-]+):',line)
        if label:current=label.group(1);blocks[current]=[]
        elif current:blocks[current].append(line)
    if not blocks:return ['missing constructor blocks']
    bodies={name:'\n'.join(lines) for name,lines in blocks.items()}
    edges={name:re.findall(r'label %([\w.$-]+)',body) for name,body in bodies.items()}
    entry=next(iter(blocks));reachable={entry};pending=[entry]
    while pending:
        for target in edges[pending.pop()]:
            if target not in reachable:reachable.add(target);pending.append(target)
    pred={name:set() for name in reachable}
    for name in reachable:
        for target in edges[name]:pred[target].add(name)
    dom={name:({entry} if name==entry else set(reachable)) for name in reachable}
    changed=True
    while changed:
        changed=False
        for name in reachable-{entry}:
            value={name}|set.intersection(*(dom[p] for p in pred[name]))
            if value!=dom[name]:dom[name]=value;changed=True
    problems=[]
    required=['2147483647','1152921504606846975','call void @__stasha_panic(i32 6)',
              'call void @__stasha_panic(i32 8)','make.len.exceeds.cap = icmp ugt i64',
              'make.cap.unrepresentable = icmp ugt i64','make.size.overflow = icmp ugt i64']
    for item in required:
        if item not in text:problems.append('missing IR invariant: '+item)
    alloc=[name for name,body in bodies.items() if re.search(r'call ptr @malloc\(i64',body)]
    if len(alloc)!=1:return problems+['expected one retained allocation site']
    alloc=alloc[0]
    if 'scalar.ok' not in dom[alloc]:problems.append('count guard does not dominate allocation')
    if 'mul i64' not in bodies[alloc] or bodies[alloc].index('mul i64')>bodies[alloc].index('@malloc'):
        problems.append('checked allocation stride missing before allocation')
    branch=re.search(r'br i1 %make.allocation.failed, label %([\w.$-]+), label %([\w.$-]+)',bodies[alloc])
    if not branch:return problems+['missing allocation null branch']
    panic,success=branch.groups()
    if 'call void @__stasha_panic(i32 8)' not in bodies[panic] or 'unreachable' not in bodies[panic]:
        problems.append('OOM failure does not terminate with exact reason8')
    for name,body in bodies.items():
        if '@llvm.memset' in body or 'insertvalue' in body:
            if success not in dom[name]:problems.append('initialization/publication not dominated by OOM success')
        if 'trunc i64' in body and 'scalar.ok' not in dom[name]:problems.append('narrowing before count validation')
    empty=bodies.get('make.empty','')
    if not empty or re.search(r'call|load|store|getelementptr',empty):problems.append('empty path performs memory operation')
    if '[ zeroinitializer, %make.empty ]' not in text:problems.append('empty result is not canonical')
    return problems


def ir_checks(args,work):
    source=work/'ir-contract.sts'
    source.write_text('@[[freestanding]]; mod ir_contract; type View:[]i64;\n'
        'ext fn b1_ir(stack i64 n, stack u64 c):View {ret make.([]i64,n,c);}\n')
    output=work/'ir-contract.ll'
    proc=runner.capture([args.compiler,'ir',source,'-o=0','-o',output],work,work/'ir-contract',30)
    problems=success_gates(proc,output)
    if not problems:problems+=constructor_ir_problems(output.read_text())
    return [dict(name='constructor_ir_cfg',status='FAIL' if problems else 'PASS',problems=problems,compile=proc),
        dict(name='scalar_byte_limit_proof',status='PASS',problems=[],
             proof={str(stride):dict(max_bytes=((1<<31)-1)*stride,target_limit=(1<<63)-1,
                    fits=((1<<31)-1)*stride<=(1<<63)-1) for stride in (1,2,4,8)},
             note='For admitted scalar strides and descriptor counts, the post-count byte-overflow branch is unreachable. This is arithmetic proof coverage, not a runtime overflow claim.')]


def archive_checks(args,work):
    """Build genuine previous/current archives; never relabel old metadata."""
    results=[]
    previous=args.previous_compiler.resolve(strict=True)
    before=runner.sha256(previous)
    for version,compiler in [(4,previous),(5,args.compiler)]:
        folder=work/('archive-abi'+str(version));folder.mkdir()
        source=folder/'b1_archive.sts'
        source.write_text('mod b1_archive; ext fn answer(void):i32 { '
            'heap []i32 s=make.([]i32,1); stack i32 result=s[0]; rem.(s); ret result; }\n')
        archive=folder/'b1_archive.a'
        built=runner.capture([compiler,'lib',source,'-o',archive],folder,folder/'build-library',30,
            dict(STS_DUMP_IR_PRE=str(folder/'body-pre.ll')))
        problems=success_gates(built,archive)
        metadata=Path(str(archive)+'.stasha-abi')
        if not artifact_ok(metadata) or not metadata.read_text().startswith('STASHA_ABI '+str(version)+'\n'):
            problems.append('fresh archive has wrong semantic ABI marker')
        result=dict(name='genuine_archive_abi'+str(version),status='FAIL',problems=problems,compile=built,
                    producer=str(compiler),producer_sha256=runner.sha256(compiler),source_sha256=runner.sha256(source))
        results.append(result)
        if problems:continue
        result['archive_sha256']=runner.sha256(archive);result['metadata_sha256']=runner.sha256(metadata)
        caller=folder/'caller.sts';caller.write_text('mod caller; lib "b1_archive" from "b1_archive.a"; '
            'imp b1_archive; fn main(void):i32 { ret b1_archive.answer(); }\n')
        output=folder/'caller'
        consumed=runner.capture([args.compiler,'build',caller,'-o',output],folder,folder/'consume-library',30)
        result['consume']=consumed
        if version==4:problems+=rejection_gates(consumed,caller,output,'incompatible ABI metadata')
        else:
            problems+=success_gates(consumed,output,executable=True)
            if not problems:
                run=runner.capture([output],folder,folder/'execute',5);result['run']=run
                if run['category']!='exit' or run['returncode']!=0 or run['stdout'] or run['stderr']:
                    problems.append('fresh ABI5 archive runtime failed')
        result['status']='FAIL' if problems else 'PASS'
    if runner.sha256(previous)!=before:
        results.append(dict(name='previous_compiler_unchanged',status='FAIL',problems=['previous compiler changed']))
    return results


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler',required=True,type=Path);parser.add_argument('--cc',required=True)
    parser.add_argument('--artifacts',type=Path);parser.add_argument('--opt',default='0,1,2,3')
    parser.add_argument('--previous-compiler',type=Path,help='optional real ABI4 compiler for semantic archive migration controls')
    parser.add_argument('--case',action='append',default=[]);parser.add_argument('--list',action='store_true')
    args=parser.parse_args();selected=[c for c in cases() if not args.case or c.name in args.case]
    if not selected or set(args.case)-{c.name for c in selected}:parser.error('empty or unknown case selection')
    try:opts=[int(x) for x in args.opt.split(',')]
    except ValueError:parser.error('invalid optimization levels')
    if not opts or len(opts)!=len(set(opts)) or any(x not in range(4) for x in opts):parser.error('invalid optimization levels')
    if args.list:print(json.dumps([dict(name=c.name,outcome=c.outcome) for c in selected],indent=2));return 0
    args.compiler=args.compiler.resolve(strict=True);args.cc=runner.resolve_cc(args.cc)
    if args.artifacts:args.artifacts.mkdir(parents=True,exist_ok=True)
    work=Path(tempfile.mkdtemp(prefix='make-b1-',dir=args.artifacts)).resolve()
    source=work/'support.c';source.write_text(SUPPORT);support=work/'support.o'
    built=runner.capture([args.cc,'-std=c11','-O2','-fno-builtin','-Wall','-Wextra','-Werror','-c',source,'-o',support],work,work/'build-support',30)
    build_problems=success_gates(built,support,quiet=True)
    if build_problems:print(build_problems,built);return 1
    before=runner.sha256(args.compiler)
    summary=dict(utc=datetime.now(timezone.utc).isoformat(),compiler=str(args.compiler),compiler_sha256=before,
        cc=str(args.cc),cc_sha256=runner.sha256(args.cc),runner_sha256=runner.sha256(__file__),
        support_sha256=runner.sha256(support),results=[])
    for case in selected:
        for opt in ([0] if case.outcome=='rejection' else opts):
            r=execute_case(case,opt,args,work/(case.name+'-O'+str(opt)),support)
            summary['results'].append(r);print(r['status'],case.name,'O'+str(opt),'; '.join(r['problems']),flush=True)
    if not args.case:
        summary['results']+=cross_target_checks(args,work)
        summary['results']+=ir_checks(args,work)
        if args.previous_compiler:summary['results']+=archive_checks(args,work)
    summary['compiler_unchanged']=before==runner.sha256(args.compiler)
    summary['status']='PASS' if summary['compiler_unchanged'] and all(r['status']=='PASS' for r in summary['results']) else 'FAIL'
    summary['counts']={s:sum(r['status']==s for r in summary['results']) for s in ('PASS','FAIL')}
    (work/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(summary['status'],summary['counts'],work/'summary.json');return int(summary['status']!='PASS')


if __name__=='__main__':raise SystemExit(main())
