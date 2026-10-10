#!/usr/bin/env python3
"""C-owned pointee/member storage oracle: direct/nested/packed/array access."""
import argparse
import json
from pathlib import Path
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scalars'))
import run as runner

SOURCE = '''@[[freestanding, crash: disable]];
mod member_pointers;
imp dep;
type Inner: @c_layout struct { ext u8 tag; ext u64 value; ext u16 elems[3]; };
type Outer: @c_layout struct { ext u32 guard; ext Inner inner; ext Inner *rw link; };
type Packed: @packed struct { ext u8 tag; ext u64 value; ext u16 elems[3]; };
type PackedOuter: @packed struct { ext u8 guard; ext Packed inner; };
type FirstStatic: @comptime[T] struct { ext T value; };
fn @comptime[T] FirstStatic.maximum(void): T { stack T zero = 0; ret zero - 1; }
@[[abi: c]] ext fn first_static_u8(void): u32 { ret FirstStatic.[u8].maximum(); }
@[[abi: c]] ext fn second_static_u8(void): u32 { ret FirstStatic.[u8].maximum(); }
@[[abi: c]] ext fn first_static_u16(void): u32 { ret FirstStatic.[u16].maximum(); }
@[[abi: c]] ext fn first_static_u32(void): u64 { ret FirstStatic.[u32].maximum(); }
@[[abi: c]] ext fn read_inner(stack Inner *r p): u64 {
    ret p.value + p.tag + p.elems[0] + p.elems[2];
}
@[[abi: c]] ext fn mutate_outer(stack Outer *rw p): u64 {
    p.inner.value = 901;
    p.inner.value += 7;
    p.inner.elems[1] = 1234;
    p.inner.elems[2] += 3;
    p.link.value = 777;
    p.link.elems[0] = 4321;
    stack u64 *rw address = &p.inner.value;
    *address = *address + 5;
    stack u16 *rw element = &p.inner.elems[1];
    *element = *element + 2;
    ret p.guard + p.inner.value + p.link.value + p.inner.elems[1];
}
@[[abi: c]] ext fn mutate_packed(stack PackedOuter *rw p): u64 {
    p.inner.value = 0x123456789abcdef0;
    p.inner.value += 3;
    p.inner.elems[1] = 800;
    p.inner.elems[2] += 4;
    stack u64 *rw address = &p.inner.value;
    *address = *address + 5;
    ret p.inner.value;
}
@[[abi: c]] ext fn read_packed(stack PackedOuter *r p): u64 {
    ret p.inner.value + p.inner.tag + p.inner.elems[0];
}
type Factory: struct {
    ext i64 value;
    ext fn get(void): i64 { ret this.value; }
    ext fn byte(void): u8 { ret 255; }
    ext fn add(stack i64 amount): i64 { this.value += amount; ret this.get(); }
}
type FactoryHolder: struct {
    ext Factory inner;
    ext fn bump(void): i64 { ret this.inner.add(5); }
    ext fn wide_byte(void): u32 { ret this.inner.byte(); }
}
ext fn Factory.from_value(stack i64 value, stack u8 add): Factory {
    ret Factory{.value = value + (i64)add};
}
@[[abi: c, export_name: "factory_c_twice"]]
ext fn Factory.twice(stack i64 value): i64 { ret value * 2; }
type Generic: @comptime[T] struct {
    ext T value;
    ext fn get(void): T { ret this.value; }
}
ext fn @comptime[T] Generic.from_value(stack T value): Generic.[T] {
    ret Generic.[T]{.value = value};
}
ext fn @comptime[T] Generic.empty(void): Generic.[T] {
    ret Generic.[T]{.value = 0};
}
@[[abi: c]] ext fn static_factories(void): i32 {
    stack Factory f = Factory.from_value(-7, 251);
    if f.get() != 244 || f.add(5) != 249 { ret 1; }
    stack FactoryHolder holder = .{.inner = f};
    if holder.bump() != 254 || holder.inner.value != 254 || holder.wide_byte() != 255 { ret 5; }
    stack Factory g = f.from_value(-9, 11);
    if g.get() != 2 || Factory.twice(-29) != -58 { ret 2; }
    stack Generic.[i64] generic = Generic.[i64].from_value(-100);
    stack Generic.[u8] small = Generic.[u8].from_value(250);
    stack Generic.[i64] empty = Generic.[i64].empty();
    if generic.get() != -100 || small.get() != 250 || empty.get() != 0 { ret 3; }
    stack Builder imported = dep:Builder:from_value(19);
    if imported.get() != 19 || dep:Builder:company() != 41 { ret 4; }
    ret 0;
}

'''
DRIVER = '''#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint8_t tag; uint64_t value; uint16_t elems[3]; } Inner;
typedef struct { uint32_t guard; Inner inner; Inner *link; } Outer;
typedef struct __attribute__((packed)) { uint8_t tag; uint64_t value; uint16_t elems[3]; } Packed;
typedef struct __attribute__((packed)) { uint8_t guard; Packed inner; } PackedOuter;
extern uint64_t read_inner(const Inner *), mutate_outer(Outer *);
extern uint64_t mutate_packed(PackedOuter *), read_packed(const PackedOuter *);
extern int32_t static_factories(void);
extern int64_t factory_c_twice(int64_t);
extern uint32_t first_static_u8(void), second_static_u8(void), first_static_u16(void);
extern uint64_t first_static_u32(void);
int main(void) {
    if (first_static_u8() != 255 || second_static_u8() != 255 ||
        first_static_u16() != 65535 || first_static_u32() != UINT64_C(4294967295)) return 8;
    int64_t (*indirect)(int64_t) = factory_c_twice;
    if (static_factories() != 0 || factory_c_twice(-71) != -142 || indirect(103) != 206) return 7;
    Inner linked = {19, 23, {11, 12, 13}};
    Outer value = {81, {17, 100, {3, 5, 7}}, &linked};
    if (read_inner(&value.inner) != 127) return 1;
    if (mutate_outer(&value) != 81+913+777+1236) return 2;
    if (value.guard != 81 || value.inner.tag != 17 || value.inner.value != 913 ||
        value.inner.elems[0] != 3 || value.inner.elems[1] != 1236 || value.inner.elems[2] != 10 ||
        value.link != &linked || linked.tag != 19 || linked.value != 777 ||
        linked.elems[0] != 4321 || linked.elems[1] != 12 || linked.elems[2] != 13) return 3;
    /* Deliberately offset the entire packed object by one byte. */
    uint8_t raw[sizeof(PackedOuter)+2]; memset(raw, 0xa5, sizeof raw);
    PackedOuter original = {47, {31, 400, {20, 30, 40}}};
    memcpy(raw+1, &original, sizeof original);
    PackedOuter *packed = (PackedOuter *)(void *)(raw+1);
    if (read_packed(packed) != 451) return 4;
    if (mutate_packed(packed) != UINT64_C(0x123456789abcdef8)) return 5;
    if (packed->guard != 47 || packed->inner.tag != 31 ||
        packed->inner.value != UINT64_C(0x123456789abcdef8) || packed->inner.elems[0] != 20 ||
        packed->inner.elems[1] != 800 || packed->inner.elems[2] != 44 ||
        raw[0] != 0xa5 || raw[sizeof raw-1] != 0xa5) return 6;
    puts("MEMBER_POINTERS_OK"); return 0;
}
'''
DEP = """mod dep;
ext type Builder: struct {
    ext i64 value;
    ext fn get(void): i64 { ret this.value; }
}
ext fn Builder.from_value(stack i64 value): Builder { ret Builder{.value = value}; }
ext fn Builder.company(void): i64 { ret 41; }
"""
NEGATIVES = {
    # Address derivation is rejected before a member read/write is attempted.
    'readonly_address_widening': ('type Box: struct { ext i32 x; }; fn bad(stack Box *r p): i32 *rw { ret &p.x; }', 'cannot derive a writable address from read-only, const or final storage'),
    'readonly_conditional_address_widening': ('type Box: struct { ext i32 x; }; fn bad(stack Box *r p, stack Box *r q, stack bool c): i32 *rw { ret c ? &p.x : &q.x; }', 'cannot derive a writable address from read-only, const or final storage'),
    'object_static_variadic_missing_fixed': ('type Box: struct { ext i32 x; }; fn Box.create(stack i32 n, ...): i32 { ret n; } fn main(void): i32 { stack Box b = .{}; ret b.create(); }', 'method argument count does not match'),
    'readonly_explicit_deref_write': ('type Box: struct { ext i32 x; }; fn bad(stack Box *r p): void { (*p).x = 1; }', 'requires a writable pointer'),
    'writeonly_field_read': ('type Box: struct { ext i32 x; }; fn bad(stack Box *w p): i32 { ret p.x; }', 'requires a readable pointer'),
    'double_pointer_member': ('type Box: struct { ext i32 x; }; fn bad(stack Box *rw *rw p): i32 { ret p.x; }', 'multiple pointer levels requires an explicit dereference'),
    'colon_instance_call': ('imp dep; fn main(void): i32 { dep:Builder:get(); ret 0; }', 'instance method requires an object receiver'),
    'generic_instance_call': ('type Box: @comptime[T] struct { ext T x; ext fn get(void): T { ret this.x; } }; fn main(void): i32 { Box.[i32].get(); ret 0; }', 'instance method requires an object receiver'),
    'external_static_this': ('type Box: struct { ext i32 x; }; fn Box.bad(void): i32 { ret this.x; }', "'this' used outside of a struct body method"),
    'generic_static_this': ('type Box: @comptime[T] struct { ext T x; }; fn @comptime[T] Box.bad(void): T { ret this.x; } fn main(void): i32 { ret Box.[i32].bad(); }', "'this' used outside of a struct body method"),
    'shorthand_constructor_missing_argument': ('type Box: struct { ext i32 x; }; fn Box.new(stack i32 x): Box { ret Box{.x=x}; } fn main(void): i32 { stack Box b = Box.(); ret 0; }', 'method argument count does not match'),
    'type_instance_call': ('type Box: struct { ext i32 x; ext fn rem(void): void { this.x = 0; } }; fn main(void): i32 { Box.rem(); ret 0; }', 'instance method requires an object receiver'),
    'readonly_field_write': ('type Box: struct { ext i32 x; }; fn bad(stack Box *r p): void { p.x = 1; }', 'requires a writable pointer'),
    'readonly_nested_write': ('type Inner: struct { ext i32 x; }; type Box: struct { ext Inner value; }; fn bad(stack Box *r p): void { p.value.x = 1; }', 'requires a writable pointer'),
    'readonly_array_write': ('type Box: struct { ext i32 values[2]; }; fn bad(stack Box *r p): void { p.values[0] = 1; }', 'requires a writable pointer'),
    'constructor_missing_argument': ('type Box: struct { ext i32 x; }; fn Box.new(stack i32 x): Box { ret Box{.x=x}; } fn main(void): i32 { stack Box b = Box.new(); ret 0; }', 'method argument count does not match'),
    'constructor_extra_argument': ('type Box: struct { ext i32 x; }; fn Box.new(stack i32 x): Box { ret Box{.x=x}; } fn main(void): i32 { stack Box b = Box.new(1,2); ret 0; }', 'method argument count does not match'),
}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--compiler',type=Path,required=True)
    ap.add_argument('--cc',default='cc')
    ap.add_argument('--artifacts',type=Path,required=True)
    a=ap.parse_args(); a.artifacts.mkdir(parents=True,exist_ok=True)
    root=Path(tempfile.mkdtemp(prefix='member-pointers-',dir=a.artifacts)); compiler=a.compiler.resolve(); rows=[]
    for opt in range(4):
        work=root/f'O{opt}';work.mkdir();source=work/'member.sts';source.write_text(SOURCE)
        (work/'dep.sts').write_text(DEP)
        driver=work/'driver.c';driver.write_text(DRIVER);obj=work/'member.o';exe=work/'program';problems=[];processes=[]
        for stage,cmd in [('compile',[compiler,'obj',source,f'-o={opt}','-o',obj]),('link',[a.cc,'-std=c11','-Wall','-Wextra','-Werror',driver,obj,'-o',exe]),('run',[exe])]:
            p=runner.capture(cmd,work,work/stage,60);processes.append(p)
            if p['category']!='exit' or p['returncode']!=0 or runner.INTERNAL_FAILURE.search(p['stderr']) or runner.WARNING.search(p['stderr']):problems.append(stage+' failed');break
            if stage=='run' and (p['stdout']!='MEMBER_POINTERS_OK\n' or p['stderr']):problems.append('exact output mismatch')
        rows.append(dict(name=f'members-O{opt}',problems=problems,processes=processes));print('FAIL' if problems else 'PASS',rows[-1]['name'])
        for name,(body,diagnostic) in NEGATIVES.items():
            src=work/(name+'.sts');out=work/(name+'.o');src.write_text('@[[freestanding, crash: disable]];\nmod negative;\n'+body+'\n')
            p=runner.capture([compiler,'obj',src,f'-o={opt}','-o',out],work,work/name,60)
            ok=p['category']=='exit' and p['returncode']!=0 and diagnostic in p['stderr'] and not out.exists() and not runner.INTERNAL_FAILURE.search(p['stderr'])
            rows.append(dict(name=f'{name}-O{opt}',problems=[] if ok else ['negative oracle mismatch'],processes=[p]));print('PASS' if ok else 'FAIL',rows[-1]['name'])
    summary=dict(compiler=str(compiler),compiler_sha256=runner.sha256(compiler),source_sha256=runner.sha256(Path(__file__)),rows=rows)
    (root/'summary.json').write_text(json.dumps(summary,indent=2)+'\n'); print(root/'summary.json')
    return int(any(r['problems'] for r in rows))
if __name__=='__main__':raise SystemExit(main())
