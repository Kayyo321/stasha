#!/usr/bin/env python3
"""Full-compiler layout tests. Fresh artifacts; no stale binaries or xfails.
The executable interface is scalar/pointer-only. This tests object representation,
not aggregate argument/return ABI qualification.
"""
from __future__ import annotations
import argparse
import itertools
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HEADER = '@[[freestanding, crash: disable]];\n'
TYPES = [
    ('u8', 'uint8_t', '3'), ('u16', 'uint16_t', '1003'),
    ('u32', 'uint32_t', '100003'), ('u64', 'uint64_t', '100000003'),
    ('f32', 'float', '3.25'), ('f64', 'double', '19.5'),
]

def run(cmd, *, expected=0, env=None):
    p = subprocess.run([str(c) for c in cmd], cwd=ROOT, text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=120, env=env)
    if p.returncode != expected:
        raise AssertionError(f'command returned {p.returncode}, expected {expected}: {cmd}\n{p.stdout}')
    return p.stdout

def generate(directory):
    sts = [HEADER]
    c = ['#include <stdint.h>\n#include <stddef.h>\n#include <stdio.h>\n#include <string.h>\n']
    calls = []
    for index, (packed, a, b) in enumerate(itertools.product((False, True), TYPES, TYPES)):
        name = f'Pair{index}'
        attr = '@packed' if packed else '@c_layout'
        cattr = '__attribute__((packed))' if packed else ''
        sts.append(f'type {name}: {attr} struct {{ ext {a[0]} a; ext {b[0]} b; }};\n')
        sts.append(f'ext fn check{index}(stack void *p, stack u64 size): i32;\n')
        sts.append(f'ext fn probe{index}(void): i32 {{ stack {name} x = .{{}}; x.a = {a[2]}; x.b = {b[2]}; ret check{index}(&x, sizeof.({name})); }}\n')
        c.append(f'struct {cattr} {name} {{ {a[1]} a; {b[1]} b; }};\n')
        c.append(f'int32_t check{index}(const void *p, uint64_t size) {{ struct {name} x; if (size != sizeof x) return 1; memcpy(&x,p,sizeof x); return x.a != {a[2]} || x.b != {b[2]}; }}\n')
        c.append(f'extern int32_t probe{index}(void);\n')
        calls.append(f'if(probe{index}()) {{ fprintf(stderr,"pair {index} failed\\n"); return 1; }}')
    sts.append('''
// These definitions intentionally precede their by-value dependencies.
type Outer: struct { ext u8 prefix; ext Choice values[2][3]; ext u8 suffix; };
type Event: enum { Empty, Data(stack Large) };
type Choice: union { ext Large large; ext u64 words[12]; ext u8 bytes[99]; };
type Large: @c_layout struct { ext u8 bytes[65]; ext u64 last; };
type Node: struct { ext Node *next; ext u64 value; };
type Generic: @comptime[T] @c_layout struct { ext u8 tag; ext T values[3]; };
type NaturalAligned: @align(8) struct { ext u8 tag; ext u64 value; };
type SmallAlignment: @align(1) struct { ext u8 tag; ext u64 value; };
type Packed: @packed struct {
    ext u8 tag; ext u64 value;
    ext fn bump(void): u64 { this.value += 1; ret this.value; }
};
type PackedOuter: @packed struct { ext u8 first; ext Packed inner; };
ext fn check_extra(stack void *p, stack u64 a, stack u64 b, stack u64 c, stack u64 d, stack u64 e, stack u64 f): i32;
ext fn probe_extra(void): i32 {
    stack Large large = .{};
    large.last = 123456789;
    stack Event event = Event.Data(large);
    stack Generic.[u64] generic = .{};
    ret check_extra(&event, sizeof.(Event), sizeof.(Outer), sizeof.(Choice), sizeof.(generic), sizeof.(NaturalAligned), sizeof.(SmallAlignment));
}
ext fn probe_packed(void): u64 {
    stack Packed p = .{};
    p.tag = 7;
    p.value = 100;
    p.value += 1;
    stack u64 result = p.bump();
    stack PackedOuter outer = .{};
    outer.inner = p;
    ret result + outer.inner.value;
}
''')
    c.append('''
struct Large { uint8_t bytes[65]; uint64_t last; };
union Choice { struct Large large; uint64_t words[12]; uint8_t bytes[99]; };
struct Event { int32_t tag; union { struct Large data; } payload; };
struct Outer { uint8_t prefix; union Choice values[2][3]; uint8_t suffix; };
struct Generic { uint8_t tag; uint64_t values[3]; };
struct NaturalAligned { uint8_t tag; uint64_t value; };
int32_t check_extra(const void *p, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    struct Event event;
    if(a!=sizeof event || b!=sizeof(struct Outer) || c!=sizeof(union Choice) ||
       d!=sizeof(struct Generic) || e!=sizeof(struct NaturalAligned) || f!=sizeof(struct NaturalAligned)) return 1;
    memcpy(&event,p,sizeof event);
    return event.tag!=1 || event.payload.data.last!=123456789;
}
extern int32_t probe_extra(void);
extern uint64_t probe_packed(void);
''')
    c.append('int main(void) {\n'+'\n'.join(calls)+'\nif(probe_extra() || probe_packed()!=204) { return 1; }\nputs("layout runtime oracle passed"); return 0; }\n')
    (directory/'matrix.sts').write_text(''.join(sts))
    (directory/'oracle.c').write_text(''.join(c))
    return directory/'matrix.sts', directory/'oracle.c'

NEGATIVES = {
    'overalign16': ('type S: @align(16) struct { ext u64 a; };', 'over-alignment is not supported'),
    'overalign32': ('type S: @align(32) struct { ext u64 a; };', 'over-alignment is not supported'),
    'overalign64': ('type S: @align(64) struct { ext u64 a; };', 'over-alignment is not supported'),
    'invalid_align': ('type S: @align(3) struct { ext u64 a; };', 'power of two'),
    'zero_align': ('type S: @align(0) struct { ext u8 a; };', 'alignment'),
    'bitfield': ('type S: struct { ext u64 flags: 64; };', 'bitfield'),
    'bitfield_zero': ('type S: struct { ext u64 flags: 0; };', 'bitfield'),
    'recursive': ('type A: struct { ext B b; }; type B: struct { ext A a; };', 'recursive by-value'),
    'recursive_enum': ('type A: enum { Value(stack B) }; type B: struct { ext A a; };', 'recursive by-value'),
    'unknown': ('type A: struct { ext Unknown value; };', 'unknown'),
    'unsized': ('type A: struct { ext void value; };', 'unsized'),
    'zero_array': ('type A: struct { ext u64 value[0]; };', 'positive literal extent'),
    'huge_array': ('type A: struct { ext u64 value[9223372036854775807]; };', 'overflows'),
    'ignored_enum_repr': ('type A: @packed enum { One, Two };', 'representation attributes'),
}

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--compiler',type=Path,default=ROOT/'bin/stasha')
    ap.add_argument('--cc',default=os.environ.get('CC','cc'))
    ap.add_argument('--target',default='x86_64-unknown-linux-gnu')
    ap.add_argument('--generate-only',type=Path)
    args=ap.parse_args()
    if args.generate_only:
        args.generate_only.mkdir(parents=True,exist_ok=True)
        generate(args.generate_only)
        for name,(source,_) in NEGATIVES.items():
            (args.generate_only/(name+'.sts')).write_text(HEADER+source+'\n')
        return
    compiler=args.compiler.resolve()
    if not compiler.is_file(): raise SystemExit(f'Missing compiler: {compiler}; build pinned toolchain first')
    with tempfile.TemporaryDirectory(prefix='stasha-layout-') as temp:
        d=Path(temp); source,c=generate(d)
        for opt in (0,2):
            obj=d/f'layout-O{opt}.o'; ir=d/f'layout-O{opt}.ll'; binary=d/f'oracle-O{opt}'
            common=[source,'--target',args.target,f'-o={opt}']
            run([compiler,'obj',*common,'-o',obj]); assert obj.is_file() and obj.stat().st_size
            run([compiler,'ir',*common,'-g','-o',ir]); assert ir.is_file() and ir.stat().st_size
            text=ir.read_text()
            if opt==0:
                accesses=[line for line in text.splitlines() if re.search(r'\b(load|store)\b',line) and 'atomic' not in line]
                assert accesses and all(re.search(r'\balign 1\b',line) for line in accesses), 'ordinary access overclaims alignment'
                assert 'DICompositeType' in text and 'DW_TAG_array_type' in text, 'missing array debug type'
                assert re.search(r'name: "b".*offset: 64',text), 'missing natural field debug offset'
                assert re.search(r'name: "b".*offset: 8',text), 'missing packed field debug offset'
            run([args.cc,'-std=c11','-Wall','-Wextra','-Werror',c,obj,'-o',binary])
            print(run([binary]).strip(),f'O{opt}')
        for name,(body,diagnostic) in NEGATIVES.items():
            path=d/(name+'.sts'); path.write_text(HEADER+body+'\n'); obj=d/(name+'.o')
            obj.write_bytes(b'STALE OUTPUT MUST BE REMOVED')
            p=subprocess.run([str(compiler),'obj',str(path),'--target',args.target,'-o',str(obj)],cwd=ROOT,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=120)
            assert p.returncode==1, f'{name}: expected clean diagnostic exit1, got {p.returncode}\n{p.stdout}'
            assert diagnostic in p.stdout, f'{name}: missing {diagnostic!r}\n{p.stdout}'
            assert not obj.exists(), f'{name}: stale output survived compilation failure'
        print(f'PASS: 72 field-pair runtime cases, nested/generic/tagged/packed cases at O0/O2; {len(NEGATIVES)} rejected layouts')
if __name__=='__main__': main()
