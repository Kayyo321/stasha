#!/usr/bin/env python3
"""GNU/Linux x86-64 C differential call ABI tests, independently of layout.
Fresh objects/executables only. The C compiler is the primary ABI oracle.
C and Stasha call each other, directly, indirectly and through callbacks.
The first executable phase checks object layout over scalar/pointer interfaces;
the second phase checks aggregate calls. No ABI success is inferred from layout.
"""
from __future__ import annotations
import argparse, hashlib, itertools, json, os, platform, subprocess, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
TYPES = [('u8','uint8_t','3'),('u16','uint16_t','1003'),('u32','uint32_t','100003'),
         ('u64','uint64_t','100000003'),('f32','float','3.25'),('f64','double','19.5')]
def run(cmd, *, env=None):
    p = subprocess.run([str(c) for c in cmd], cwd=ROOT, text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=180, env=env)
    if p.returncode: raise AssertionError(f'{p.returncode}: {cmd}\n{p.stdout}')
    return p.stdout

def generate(d):
    sts = ['@[[freestanding, crash: disable]];\n']
    c = ['#include <stdint.h>\n#include <stddef.h>\n#include <stdio.h>\n#include <string.h>\n']
    layouts, calls = [], []
    def case(name, stbody, cbody, assigns, condition, transform, packed=False, union=False):
        attr = '@packed' if packed else '@c_layout'
        kind = 'union' if union else 'struct'
        cattr = '__attribute__((packed))' if packed else ''
        sts.append(f'type {name}: {attr} {kind} {{ {stbody} }};\n')
        c.append(f'typedef {kind} {cattr} {{ {cbody} }} {name};\n')
        sts.extend([f'@[[abi: c]] ext fn c_{name}(stack {name} x): {name};\n',
                    f'@[[abi: c]] ext fn layout_{name}(stack void *p, stack u64 size): i32;\n',
                    f'@[[abi: c]] ext fn callback_{name}(stack void *p): i32;\n',
                    f'@[[abi: c]] ext fn sts_{name}(stack {name} x): {name} {{ {transform} ret x; }}\n',
                    f'@[[abi: c]] ext fn layout_probe_{name}(void): i32 {{ stack {name} x = .{{}}; {assigns} ret layout_{name}(&x, sizeof.({name})); }}\n',
                    f'@[[abi: c]] ext fn probe_{name}(void): i32 {{ stack {name} x = .{{}}; {assigns} stack let indirect = c_{name}; x = indirect(x); if {condition} {{ ret 1; }} ret callback_{name}(&sts_{name}); }}\n'])
        # Language field access/assign/compare spelling is shared by C in these fixtures.
        c.extend([f'{name} c_{name}({name} x) {{ {transform} return x; }}\n',
                  f'extern {name} sts_{name}({name});\n',
                  f'extern int32_t probe_{name}(void), layout_probe_{name}(void);\n',
                  f'int32_t layout_{name}(void *p, uint64_t size) {{ {name} want={{0}}, x; if(size!=sizeof x) return 1; memcpy(&x,p,sizeof x);\n'])
        # Compare member values, never indeterminate padding.
        c.append(assigns.replace('x.', 'want.'))
        paths = [part.strip().split(' = ')[0].removeprefix('x.') for part in assigns.split(';') if part.strip()]
        c.append('return ' + ' || '.join(f'x.{path} != want.{path}' for path in paths) + '; }\n')
        c.append(f'int32_t callback_{name}(void *p) {{ {name} (*f)({name}) = ({name}(*)({name}))p; {name} x={{0}}; {assigns} x=f(x); return {condition}; }}\n')
        layouts.append(f'if(layout_probe_{name}()) {{ puts("layout:{name}"); return 1; }}')
        calls.append(f'if(probe_{name}()) {{ puts("call:{name}"); return 2; }}')
        calls.append(f'{{ {name} x={{0}}; {assigns} x=sts_{name}(x); if({condition}) {{ puts("export:{name}"); return 3; }} }}')
    for i,(packed,a,b) in enumerate(itertools.product((False,True),TYPES,TYPES)):
        case(f'Pair{i}',f'ext {a[0]} a; ext {b[0]} b;',f'{a[1]} a; {b[1]} b;',
             f'x.a = {a[2]}; x.b = {b[2]};',f'x.a != {float(a[2])+1 if a[0].startswith("f") else int(a[2])+1} || x.b != {float(b[2])+2 if b[0].startswith("f") else int(b[2])+2}',
             'x.a += 1; x.b += 2;',packed)
    case('Nested','ext Pair15 inner; ext u32 n;', 'Pair15 inner; uint32_t n;',
         'x.inner.a = 100003; x.inner.b = 100000003; x.n = 7;',
         'x.inner.a != 100004 || x.inner.b != 100000005 || x.n != 10',
         'x.inner.a += 1; x.inner.b += 2; x.n += 3;')
    # The current language's direct nested-field assignment is outside this
    # ABI suite; construct the nested field through its ordinary value slot.
    sts[:] = [s.replace('x.inner.a = 100003; x.inner.b = 100000003; x.n = 7;',
        'stack Pair15 inner = .{}; inner.a = 100003; inner.b = 100000003; x.inner = inner; x.n = 7;')
        .replace('x.inner.a += 1; x.inner.b += 2; x.n += 3;',
        'stack Pair15 inner = x.inner; inner.a += 1; inner.b += 2; x.inner = inner; x.n += 3;') for s in sts]
    case('SmallNested','ext Pair1 inner; ext f64 d;', 'Pair1 inner; double d;',
         'x.inner.a = 3; x.inner.b = 1003; x.d = 19.5;',
         'x.inner.a != 4 || x.inner.b != 1005 || x.d != 22.5',
         'x.inner.a += 1; x.inner.b += 2; x.d += 3;')
    sts[:] = [s.replace('x.inner.a = 3; x.inner.b = 1003; x.d = 19.5;',
        'stack Pair1 inner = .{}; inner.a = 3; inner.b = 1003; x.inner = inner; x.d = 19.5;')
        .replace('x.inner.a += 1; x.inner.b += 2; x.d += 3;',
        'stack Pair1 inner = x.inner; inner.a += 1; inner.b += 2; x.inner = inner; x.d += 3;') for s in sts]
    case('Floats3','ext f32 a; ext f32 b; ext f32 c;', 'float a,b,c;',
         'x.a = 1.25; x.b = 2.5; x.c = 3.75;', 'x.a != 2.25 || x.b != 4.5 || x.c != 6.75',
         'x.a += 1; x.b += 2; x.c += 3;')
    case('Array3','ext u16 v[3];','uint16_t v[3];',
         'x.v[0] = 101; x.v[1] = 202; x.v[2] = 303;', 'x.v[0] != 102 || x.v[1] != 204 || x.v[2] != 306',
         'x.v[0] = x.v[0] + 1; x.v[1] = x.v[1] + 2; x.v[2] = x.v[2] + 3;')
    case('Large','ext u64 a; ext u64 b; ext u64 c;','uint64_t a,b,c;',
         'x.a = 1001; x.b = 2002; x.c = 3003;', 'x.a != 1002 || x.b != 2004 || x.c != 3006',
         'x.a += 1; x.b += 2; x.c += 3;')
    case('UnionIntFP','ext u64 i; ext f64 f;','uint64_t i; double f;',
         'x.i = 100000000003;', 'x.i != 100000000004', 'x.i += 1;', union=True)
    case('UnionFP','ext f32 f; ext f64 d;','float f; double d;',
         'x.d = 3.25;', 'x.d != 4.25', 'x.d += 1;', union=True)
    sts.append("""
@[[abi: c]] ext fn c_i8(stack i8 x): i8;
@[[abi: c]] ext fn c_u8(stack u8 x): u8;
@[[abi: c]] ext fn sts_i8(stack i8 x): i8 { ret x; }
@[[abi: c]] ext fn sts_u8(stack u8 x): u8 { ret x; }
@[[abi: c]] ext fn narrow_probe(void): i32 {
    stack i32 a = c_i8(-101); stack i32 b = c_u8(251);
    if a != -101 || b != 251 { ret 1; } ret 0;
}
""")
    c.append("""
int8_t c_i8(int8_t x) { return x; } uint8_t c_u8(uint8_t x) { return x; }
extern int8_t sts_i8(int8_t); extern uint8_t sts_u8(uint8_t);
extern int32_t narrow_probe(void);
""")
    calls.append('if(narrow_probe() || sts_i8(-101)!=-101 || sts_u8(251)!=251) { puts("narrow integer attributes"); return 5; }')
    # Register exhaustion must roll back the whole aggregate so a later scalar
    # can still use a remaining register. Hidden sret also consumes one GPR.
    sts.append('''
@[[abi: c]] ext fn pressure_i(stack u64 a,b,c,d,e, stack Pair21 x, stack u64 z): u64;
@[[abi: c]] ext fn pressure_f(stack f64 a,b,c,d,e,f,g, stack Pair35 x, stack f64 z): f64;
@[[abi: c]] ext fn pressure_m(stack u64 a,b,c,d,e,f, stack Pair23 x, stack f64 z): f64;
@[[abi: c]] ext fn pressure_sret(stack u64 a,b,c,d,e, stack Pair21 x): Large;
@[[abi: c]] ext fn export_pressure_i(stack u64 a,b,c,d,e, stack Pair21 x, stack u64 z): u64 { ret a+b+c+d+e+x.a+x.b+z; }
@[[abi: c]] ext fn export_pressure_f(stack f64 a,b,c,d,e,f,g, stack Pair35 x, stack f64 z): f64 { ret a+b+c+d+e+f+g+x.a+x.b+z; }
@[[abi: c]] ext fn export_pressure_sret(stack u64 a,b,c,d,e, stack Pair21 x): Large {
    stack Large r = .{}; r.a = a+b+c+d+e; r.b = x.a; r.c = x.b; ret r;
}
@[[abi: c]] ext fn pressure_probe(void): i32 {
    stack Pair21 i = .{}; i.a = 101; i.b = 202;
    if pressure_i(1,2,3,4,5,i,7) != 325 { ret 1; }
    stack Pair35 f = .{}; f.a = 11.5; f.b = 12.5;
    if pressure_f(1,2,3,4,5,6,7,f,8) != 60 { ret 2; }
    stack Pair23 m = .{}; m.a = 10; m.b = 20.5;
    if pressure_m(1,2,3,4,5,6,m,8.5) != 60 { ret 3; }
    stack Large r = pressure_sret(1,2,3,4,5,i);
    if r.a != 15 || r.b != 101 || r.c != 202 { ret 4; }
    ret 0;
}
''')
    c.append('''
uint64_t pressure_i(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,Pair21 x,uint64_t z) { return a+b+c+d+e+x.a+x.b+z; }
double pressure_f(double a,double b,double c,double d,double e,double f,double g,Pair35 x,double z) { return a+b+c+d+e+f+g+x.a+x.b+z; }
double pressure_m(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,Pair23 x,double z) { return a+b+c+d+e+f+x.a+x.b+z; }
Large pressure_sret(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,Pair21 x) { return (Large){a+b+c+d+e,x.a,x.b}; }
extern int32_t pressure_probe(void);
extern uint64_t export_pressure_i(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,Pair21,uint64_t);
extern double export_pressure_f(double,double,double,double,double,double,double,Pair35,double);
extern Large export_pressure_sret(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,Pair21);
''')
    calls.append('if(pressure_probe()) { puts("register pressure"); return 4; }')
    calls.append('if(export_pressure_i(1,2,3,4,5,(Pair21){101,202},7)!=325 || export_pressure_f(1,2,3,4,5,6,7,(Pair35){11.5,12.5},8)!=60) { puts("export register pressure"); return 6; }')
    calls.append('{ Large x=export_pressure_sret(1,2,3,4,5,(Pair21){101,202}); if(x.a!=15 || x.b!=101 || x.c!=202) { puts("export sret register pressure"); return 7; } }')
    c.append('int main(void) {\n'+'\n'.join(layouts)+'\nputs("object layout: 79 cases passed");\n'+'\n'.join(calls)+'\nputs("aggregate call ABI: 79 bidirectional/callback/indirect cases and 7 register pressure cases passed"); return 0; }\n')
    (d/'matrix.sts').write_text(''.join(sts)); (d/'oracle.c').write_text(''.join(c))

def reject_cases(compiler, out):
    prefix = '@[[freestanding, crash: disable]];\ntype P: struct { ext i32 a; ext i32 b; };\n'
    fixtures = {
        'foreign_target': (prefix + '@[[abi: c]] ext fn f(stack P x): P;\n',
            ['--target', 'aarch64-unknown-linux-gnu'], 'qualified only'),
        'erased_pointer': (prefix + '@[[abi: c]] ext fn f(stack P x): P;\n'
            'fn probe(void): void { stack fn*(stack P): P p = f; }\n',
            [], 'erase the calling convention'),
        'aggregate_varargs': (prefix + '@[[abi: c]] ext fn f(stack i32 n, ...): i32;\n'
            'fn probe(void): i32 { stack P p = .{}; ret f(1, p); }\n',
            [], 'aggregate variadic arguments'),
        'inferred_signature': (prefix + 'lib "not_linked" = c;\n'
            'fn probe(void): i32 { stack P p = .{}; ret c.f(p); }\n',
            [], 'requires a declared C signature'),
        'c_array_parameter': (prefix + '@[[abi: c]] ext fn f(stack i32 x[3]): i32;\n',
            [], 'array parameters must be declared as pointers'),
        'wrong_pointer_signature': (prefix + '@[[abi: c]] ext fn f(stack i32 x): i32;\n'
            'fn probe(void): void { stack fn*(stack f64): f64 p = f; }\n',
            [], 'does not match the declared C signature'),
        'bitfield': ('@[[freestanding, crash: disable]];\ntype B: struct { ext u32 bits: 3; };\n'
            '@[[abi: c]] ext fn f(stack B x): B;\n',
            [], 'bitfield access/layout is not qualified'),
    }
    for name, (source, flags, text) in fixtures.items():
        src = out / (name + '.sts'); obj = out / (name + '.o'); src.write_text(source)
        p = subprocess.run([str(compiler),'obj',str(src),'-o',str(obj),*flags],cwd=ROOT,
            stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=120)
        (out/(name+'.log')).write_text(p.stdout)
        if p.returncode == 0 or text not in p.stdout or obj.exists():
            raise AssertionError(f'negative ABI case {name} failed its oracle: {p.returncode}\n{p.stdout}')
    return len(fixtures)

def main():
    ap=argparse.ArgumentParser();ap.add_argument('compiler',type=Path);ap.add_argument('--cc',default='clang');ap.add_argument('--out',type=Path)
    args=ap.parse_args(); compiler=args.compiler.resolve()
    if platform.system()!='Linux' or platform.machine() not in ('x86_64','amd64'): raise SystemExit('requires GNU/Linux x86-64 LP64 execution')
    out=args.out or Path(tempfile.mkdtemp(prefix='stasha-sysv-abi-'));out.mkdir(parents=True,exist_ok=True);generate(out)
    report={'compiler':str(compiler),'sha256':hashlib.sha256(compiler.read_bytes()).hexdigest(),'c_compiler':run([args.cc,'--version']),'optimizations':[]}
    for opt in (0,2):
        obj=out/f'stasha-O{opt}.o';exe=out/f'matrix-O{opt}'
        env=dict(os.environ,STS_DUMP_IR_PRE=str(out/f'pre-O{opt}.ll'),STS_DUMP_IR=str(out/f'post-O{opt}.ll'))
        run([compiler,'obj',out/'matrix.sts',f'-o={opt}','-o',obj],env=env)
        run([args.cc,f'-O{opt}',out/'oracle.c',obj,'-o',exe])
        ir=(out/f'pre-O{opt}.ll').read_text()
        for expected in ('define signext i8 @sts_i8(i8 signext', 'define zeroext i8 @sts_u8(i8 zeroext', 'sret(%Large)', 'byval(%Large)'):
            if expected not in ir: raise AssertionError(f'missing ABI lowering: {expected}')
        result=run([exe]); print(f'O{opt}: {result}',end='');report['optimizations'].append({'level':opt,'output':result})
    report['rejections'] = reject_cases(compiler, out)
    print(f"Explicit ABI boundary rejections: {report['rejections']} passed")
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(f'Artifacts: {out}')
if __name__=='__main__':main()
