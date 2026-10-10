#!/usr/bin/env python3
"""A1 explicit checked access qualification, independent of allocation safety.

Runtime failures use live backing and an exact strong reason-6 hook. Wide or
negative boundary probes form addresses only (never dereference them). Compile
negatives remain compile-only even if accidentally accepted. The C support can
corrupt numerical descriptor fields without invalidating its backing allocation,
and verifies operand traces before acknowledging a bounds panic.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import tempfile
import run_access_contracts as access

ROOT = access.ROOT
DECL = '''type View: []i32;
ext fn bounds_shape(stack void *rw p, stack i32 length, stack i32 capacity): void;
ext fn bounds_expect(stack i32 expected): void;
ext fn bounds_step(stack i32 label, stack i32 value): i32;
ext fn bounds_trace(void): i32;
fn get(stack View view, stack i32 label): View { bounds_step(label, 0); ret view; }
'''
SUPPORT = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int trace, expected = -1;
struct view { void *data; int32_t len, cap; };
void bounds_shape(void *p, int32_t len, int32_t cap) {
    struct view v; memcpy(&v, p, sizeof v); v.len=len; v.cap=cap; memcpy(p, &v, sizeof v);
}
void bounds_expect(int value) { expected=value; trace=0; }
int bounds_step(int label, int value) { trace=trace*10+label; return value; }
int bounds_trace(void) { return trace; }
_Noreturn void __stasha_panic(uint32_t reason) {
    if (reason != 6 || (expected >= 0 && trace != expected)) {
        fprintf(stderr, "BOUNDS_HOOK_MISMATCH:reason=%u:trace=%d:expected=%d\n", reason, trace, expected);
        _Exit(87);
    }
    fputs("STASHA_TRAP:bounds\n", stderr); fflush(stderr); _Exit(86);
}
'''

def cases():
    out=[]
    def add(name, body, decl='', trap=False, diagnostic=None):
        out.append(access.Case(name, access.program(name, body, DECL+decl),
            outcome='rejection' if diagnostic else 'trap' if trap else 'success',
            reason='bounds' if trap else '', diagnostic=diagnostic or ''))
    # Arrays embedded in a larger, fully initialized object make omitted
    # immediate one-past guards observable without a read beyond backing bytes.
    box='type Box: struct { ext i32 before; ext i32 a[2]; ext i32 after; };'
    setup='stack Box b = .{.before = 5, .a = .{7,8}, .after = 9};\n'
    for op,stmt in [('read','if b.a[i] != 9 { ret 1; }'),('store','b.a[i] = 10;'),('compound','b.a[i] += 1;')]:
        add('array_oob_'+op,setup+'stack i32 i=2; '+stmt+' ret 0;',box,trap=True)
        add('array_unsafe_oob_'+op,setup+'stack i32 i=2; unsafe { '+stmt+' } ret 0;',box,trap=True)
    for typ,val in [('i8','-1'),('i16','-1'),('i32','-1'),('i64','-1'),('u8','255'),('u16','65535'),
                    ('u32','4294967295'),('u64','4294967296'),('u64','4294967297'),('u64','18446744073709551615')]:
        for source in ('a','s'):
            name='wide_'+source+'_'+typ+'_'+val.replace('-','neg')
            add(name,'stack i32 a[2]=.{7,8}; stack []i32 _s=a[:];\n'.replace('_s','s' if source=='s' else '_s')+
                f'stack {typ} i={val}; stack i32 *rw _p=&{source}[i]; ret 0;',trap=True)
    add('array_valid_all_widths','''stack i32 a[256]=.{};
stack u8 u=128; stack i64 last=255; a[u]=7; a[u]+=1; a[last]=9;
stack i32 *rw p=&a[u]; if p[0]!=8 || a[last]!=9 { ret 1; }
stack i32 *rw one=&a[256]; if one==nil { ret 2; } ret 0;''')
    add('terminal_array_one_past','stack i32 a[2]=.{7,8}; stack i32 *rw p=&a[2]; if p==nil { ret 1; } ret 0;')
    add('terminal_nested_one_past','stack i32 a[2][2]=.{[0]=.{1,2},[1]=.{3,4}}; stack i32 *rw p=&a[1][2]; if p==nil { ret 1; } ret 0;')
    add('nonterminal_nested_one_past','stack i32 a[2][2]=.{[0]=.{1,2},[1]=.{3,4}}; stack i32 *rw _p=&a[2][0]; ret 0;',trap=True)
    add('nonterminal_field_one_past','stack Entry first=.{7}; stack Entry second=.{8}; stack Entry a[2]=.{first,second}; stack i32 *rw _p=&a[2].x; ret 0;',
        'type Entry: struct { ext i32 x; };',trap=True)
    add('terminal_slice_one_past','stack i32 a[2]=.{7,8}; stack []i32 s=a[0:1]; stack i32 *rw _p=&s[1]; ret 0;',trap=True)
    add('terminal_member_array','stack Box b=.{.before=5,.a=.{7,8},.after=9}; stack i32 *rw p=&b.a[2]; if p==nil { ret 1; } ret 0;',box)
    add('slice_rebinding','stack i32 a[2]=.{7,8}; stack []i32 s=a[:]; s=a[0:1]; stack i32 *rw _p=&s[1]; ret 0;',trap=True)
    for length,cap in [(-1,2),(1,0),(1,-1),(-1,-2)]:
        for expr in ['s[0]','s[unchecked: 0]','get(s,1)[unchecked: 0]','s[0:0]']:
            suffix=expr.replace('[','_').replace(']','').replace(':','_').replace(' ','').replace('(','_').replace(')','').replace(',','_')
            body=f'stack i32 a[2]=.{{7,8}}; stack []i32 s=a[:]; bounds_shape(&s,{length},{cap}); unsafe {{ '
            body+= ('stack []i32 _bad='+expr+';' if ':' in expr and 'unchecked' not in expr else 'stack i32 _bad='+expr+';')+' } ret 0;'
            add('shape_'+str(length).replace('-','n')+'_'+str(cap).replace('-','n')+'_'+suffix,body,trap=True)
    add('shape_checks_after_index','''stack i32 a[2]=.{7,8}; stack []i32 s=a[:]; bounds_shape(&s,-1,2);
bounds_expect(12); stack i32 _bad=get(s,1)[bounds_step(2,0)]; ret 0;''',trap=True)
    add('shape_checks_after_range_operands','''stack i32 a[2]=.{7,8}; stack []i32 s=a[:]; bounds_shape(&s,-1,2);
bounds_expect(123); stack []i32 _bad=get(s,1)[bounds_step(2,0):bounds_step(3,0)]; ret 0;''',trap=True)
    add('plain_assignment_rhs_first','''stack i32 a[2]=.{7,8}; stack []i32 s=a[0:1]; bounds_expect(312);
get(s,1)[bounds_step(2,1)]=bounds_step(3,9); ret 0;''',trap=True)
    add('compound_target_first','''stack i32 a[2]=.{7,8}; stack []i32 s=a[0:1]; bounds_expect(12);
get(s,1)[bounds_step(2,1)]+=bounds_step(3,9); ret 0;''',trap=True)
    for op,statement,expected in [('read','stack i32 value=get(s,1)[bounds_step(2,0)]; if value!=7 { ret 2; }',12),
        ('address','stack i32 *rw p=&get(s,1)[bounds_step(2,0)]; if p[0]!=7 { ret 2; }',12),
        ('store','get(s,1)[bounds_step(2,0)]=bounds_step(3,9);',312),
        ('compound','get(s,1)[bounds_step(2,0)]+=bounds_step(3,9);',123),
        ('range','stack []i32 v=get(s,1)[bounds_step(2,0):bounds_step(3,1)]; if v[0]!=7 { ret 2; }',123)]:
        add('once_order_'+op,'stack i32 a[2]=.{7,8}; stack []i32 s=a[:]; bounds_expect(-1); '+statement+
            f' if bounds_trace()!={expected} {{ ret 1; }} ret 0;')
    add('unchecked_array_live_backing',setup+'unsafe { b.a[unchecked:2]=10; b.a[unchecked:2]+=1; if b.a[unchecked:2]!=11 { ret 1; } } if b.after!=11 { ret 2; } ret 0;',box)
    add('unchecked_live_backing','stack i32 a[2]=.{7,8}; stack []i32 s=a[0:1]; unsafe { s[unchecked:1]=9; s[unchecked:1]+=1; if s[unchecked:1]!=10 { ret 1; } } if a[1]!=10 { ret 2; } ret 0;')
    add('unchecked_outside_unsafe','stack i32 a[2]=.{7,8}; ret a[unchecked:0];',diagnostic='unchecked.*unsafe')
    add('unchecked_base_locality','stack i32 a[2][2]=.{[0]=.{1,2},[1]=.{3,4}}; unsafe { stack i32 *rw _p=&a[2][unchecked:0]; } ret 0;',trap=True)
    add('unchecked_index_locality','stack i32 a[2]=.{7,8}; stack []i32 s=a[0:1]; unsafe { stack i32 *rw _p=&a[unchecked:s[1]]; } ret 0;',trap=True)
    add('unchecked_outer_locality','stack i32 a[2][2]=.{[0]=.{1,2},[1]=.{3,4}}; unsafe { stack i32 *rw _p=&a[unchecked:0][3]; } ret 0;',trap=True)
    for op in ['++','--']:
        add('indexed_update_'+('inc' if op=='++' else 'dec'),'stack i32 _a[2]=.{7,8}; _a[0]'+op+'; ret 0;',diagnostic='increment|decrement|indexed|unsupported')
    add('oversized_array_view','ret 0;', 'type Huge: struct { ext i8 a[2147483648]; }; fn view(stack Huge *r p): void { stack []i8 _v=p.a[:]; }',diagnostic='array-to-slice.*INT32_MAX')
    raw='stack i32 a[2]=.{7,8}; stack i32 *rw p=&a[0]; '
    for unsafe in (False, True):
        add('function_view_'+('unsafe' if unsafe else 'safe'),
            'stack fn*(void): i32 f=answer; '+('unsafe { ' if unsafe else '')+
            'stack []i8 _v=f[0:1]; '+('} ' if unsafe else '')+'ret 0;',
            'fn answer(void): i32 { ret 7; }',diagnostic='typed data pointer')
    add('raw_requires_unsafe',raw+'stack []i32 _v=p[0:1]; ret 0;',diagnostic='raw-pointer slicing.*unsafe')
    add('raw_requires_hi',raw+'unsafe { stack []i32 _v=p[:]; } ret 0;',diagnostic='raw-pointer slicing.*upper bound')
    add('raw_void_element','stack i32 a=7; stack void *rw p=&a; unsafe { stack []i8 _v=p[0:1]; } ret 0;',diagnostic='non-void sized element')
    add('raw_empty_at_end',raw+'unsafe { stack []i32 v=p[2:2]; if len.(v)!=0 || cap.(v)!=0 { ret 1; } } ret 0;')
    add('raw_pointer_to_slice',
        'stack i32 a[2]=.{7,8}; stack View s=a[:]; stack View *rw p=&s; '
        'unsafe { stack []View rows=p[:1]; if len.(rows)!=1 || cap.(rows)!=1 || rows[0][1]!=8 { ret 1; } } ret 0;')
    add('raw_default_lo',raw+'unsafe { stack []i32 v=p[:2]; if v[1]!=8 || cap.(v)!=2 { ret 1; } } ret 0;')
    for typ,lo,hi in [('i64','-1','0'),('i32','1','0'),('u64','0','2147483648'),('u64','4294967296','4294967297'),('u64','0','18446744073709551615')]:
        add('raw_invalid_'+typ+'_'+lo.replace('-','n')+'_'+hi,raw+f'stack {typ} lo={lo}; stack {typ} hi={hi}; unsafe {{ stack []i32 _v=p[lo:hi]; }} ret 0;',trap=True)
    add('raw_once_order',raw+'bounds_expect(-1); unsafe { stack []i32 v=raw_get(p)[bounds_step(2,0):bounds_step(3,2)]; if v[1]!=8 || len.(v)!=2 || cap.(v)!=2 { ret 1; } } if bounds_trace()!=123 { ret 2; } ret 0;',
        'fn raw_get(stack i32 *rw p): i32 *rw { bounds_step(1,0); ret p; }')
    add('raw_zero_size','stack Empty a[1]; stack Empty *rw p=&a[0]; unsafe { stack []Empty v=p[0:0]; if len.(v)!=0 || cap.(v)!=0 { ret 1; } } ret 0;', 'type Empty: struct {};')
    return out


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler',required=True,type=Path);p.add_argument('--cc',default='clang')
    p.add_argument('--opt',default='0,1,2,3');p.add_argument('--case',action='append',default=[])
    p.add_argument('--artifacts',type=Path);p.add_argument('--list',action='store_true')
    a=p.parse_args(); selected=[c for c in cases() if not a.case or c.name in a.case]
    if not selected or set(a.case)-{c.name for c in selected}: p.error('empty or unknown case selection')
    try: opts=[int(o) for o in a.opt.split(',')]
    except ValueError: p.error('optimization levels must be in 0,1,2,3')
    if not opts or len(opts)!=len(set(opts)) or any(o not in range(4) for o in opts):
        p.error('optimization levels must be distinct values in 0,1,2,3')
    if a.list: print(json.dumps([{'name':c.name,'outcome':c.outcome} for c in selected],indent=2));return 0
    a.compiler=a.compiler.resolve(strict=True);a.compile_timeout=30;a.run_timeout=5
    a.trap_exit_code=86;a.trap_signal=None;a.trap_hook_pattern='STASHA_TRAP:{reason}';a.require_trap_reasons=True
    if a.artifacts:a.artifacts.mkdir(parents=True,exist_ok=True)
    work=Path(tempfile.mkdtemp(prefix='bounds-a1-',dir=a.artifacts));support=work/'bounds_hook.c';support.write_text(SUPPORT)
    hook=work/'bounds_hook.o';cc=access.scalar_runner.resolve_cc(a.cc)
    built=access.scalar_runner.capture([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-c',support,'-o',hook],work,work/'build-hook',30)
    if built['returncode']!=0: print(built);return 1
    a.trap_hook_object=hook
    before=access.scalar_runner.sha256(a.compiler)
    summary={'utc':datetime.now(timezone.utc).isoformat(),'compiler':str(a.compiler),'compiler_sha256':before,
             'runner_sha256':access.scalar_runner.sha256(__file__),'hook_sha256':access.scalar_runner.sha256(hook),'results':[]}
    for c in selected:
        a.compiler_arg=['-l',str(hook)] if c.outcome=='success' else []
        for opt in ([0] if c.outcome=='rejection' else opts):
            result=access.scalar_runner.run_case(c,opt,a,work/(c.name+'-O'+str(opt)))
            print(result['status'],c.name,'O'+str(opt),'; '.join(result['problems']),flush=True)
            summary['results'].append(result)
    summary['compiler_unchanged']=before==access.scalar_runner.sha256(a.compiler)
    summary['status']='PASS' if summary['compiler_unchanged'] and all(r['status']=='PASS' for r in summary['results']) else 'FAIL'
    summary['counts']={s:sum(r['status']==s for r in summary['results']) for s in ('PASS','FAIL')}
    (work/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(summary['status'],summary['counts'],work/'summary.json');return int(summary['status']!='PASS')
if __name__=='__main__':raise SystemExit(main())
