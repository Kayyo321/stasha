"""Separately compiled valid native dispatch fixtures."""
import re
SCALARS=[('bool','true'),('i8','-101'),('u8','251'),('i16','-30001'),('u16','60001'),('i32','-2000000001'),('u32','4000000001'),('i64','-8000000000000000001'),('u64','16000000000000000001'),('f32','1.25'),('f64','-2.5')]
BAD=re.compile(r'warning:|error:|AddressSanitizer|runtime error:|Memory leak detected|LLVM ERROR',re.I)
def sources():
    declarations=''.join(f'echo_{t}(stack {t} v): {t};\n' for t,_ in SCALARS)
    declarations+='enum_value(stack E v): E; pointer(stack i32 *r p): i32 *r; descriptor(stack I v): I;'
    methods=''.join(f'ext fn echo_{t}(stack {t} v): {t} {{ ret v; }}\n' for t,_ in SCALARS)
    methods+='ext fn high(void): u64 { ret 16000000000000000001; }\n'
    methods+='ext fn enum_value(stack E v): E { ret v; } ext fn pointer(stack i32 *r p): i32 *r { ret p; } ext fn descriptor(stack I v): I { ret v; }'
    checks=''.join(f'if (value.echo_{t}({v}) != {v}) {{ ret {i+1}; }}\n' for i,(t,v) in enumerate(SCALARS))
    checks+='if (value.high() != 16000000000000000001) { ret 12; }\n'
    checks+='if (value.enum_value(E.B) != E.B) { ret 13; } stack i32 n=47; stack i32 *r p=value.pointer(&n); if (*p != 47) { ret 14; }\n'
    checks+='stack I copy=value.descriptor(value); if (copy.echo_i8(-101) != -101) { ret 15; } ret 0;'
    provider='mod provider; ext type E: enum { A, B }; ext type Parent: interface { high(void): u64; }; ext type I: interface.[Parent] { '+declarations+' }; ext fn run(stack I value): i32 { '+checks+' }'
    # Existing identifier-call boxing is the I1a route. Module-qualified
    # concrete-to-interface boxing is intentionally still an I1b exclusion.
    caller='mod caller; lib "provider" from "provider.a"; imp provider; type S: struct.[I] { ext i32 state; '+methods+' }; fn forward(stack I value): i32 { ret provider.run(value); } fn main(void): i32 { stack S s=.{ .state=31 }; if (s.I.echo_i8(-101) != -101) { ret 21; } if (s.Parent.high() != 16000000000000000001) { ret 22; } ret forward(s); }'
    return provider,caller
