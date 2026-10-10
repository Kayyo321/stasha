#!/usr/bin/env python3
"""Compile-only generic-source eligibility controls. No generated code runs."""
I='type I: interface { m(void): i32; }; '
GEN='type S: @comptime[T] struct.[I] { ext T x; ext fn m(void): i32 { ret 7; } }; '
LIT='type S_G_i32: struct.[I] { ext i32 x; ext fn m(void): i32 { ret 9; } }; '
CONSUME='fn consume(stack I v): i32 { ret v.m(); } '
def main(body):return 'fn main(void): i32 { '+body+' }'
def matrix():
 cases=[]
 def add(name,body,negative=True,prefix='',decls=GEN+LIT,files=None):cases.append(dict(name=name,source='mod p; '+I+decls+prefix+CONSUME+body,negative=negative,files=files or {}))
 add('generic-boxing',main('stack S.[i32] s=.{ .x=7 }; ret consume(s);'))
 add('generic-qualified',main('stack S.[i32] s=.{ .x=7 }; ret s.I.m();'))
 add('generic-alias',main('stack G s=.{ .x=7 }; ret consume(s);'),prefix='type G: S.[i32]; ')
 add('generic-alias-chain',main('stack H s=.{ .x=7 }; ret consume(s);'),prefix='type G: S.[i32]; type H: G; ')
 add('generic-designated-let',main('stack let s=S.[i32] { .x=7 }; ret consume(s);'))
 add('generic-designated-typed',main('stack S_G_i32 s=S.[i32] { .x=7 }; ret consume(s);'))
 maker='fn make(void): S.[i32] { stack S.[i32] x=.{ .x=7 }; ret x; } '
 add('generic-call-let',main('stack let s=make(); ret consume(s);'),prefix=maker)
 add('generic-call-typed',main('stack S_G_i32 s=make(); ret consume(s);'),prefix=maker)
 add('generic-identifier-copy',main('stack S.[i32] s=.{ .x=7 }; stack let copy=s; ret consume(copy);'))
 add('generic-typed-copy',main('stack S.[i32] s=.{ .x=7 }; stack S_G_i32 copy=s; ret consume(copy);'))
 add('generic-parameter','fn use(stack S.[i32] s): i32 { ret consume(s); } '+main('ret 0;'))
 add('generic-global',main('ret consume(g);'),prefix='stack S.[i32] g=.{ .x=7 }; ')
 # Global designated initialization fails in older static-initializer lowering
 # before eligibility screening; retain that characterization outside this gate.
 add('literal-boxing',main('stack S_G_i32 s=.{ .x=9 }; ret consume(s)-9;'),False)
 add('literal-qualified',main('stack S_G_i32 s=.{ .x=9 }; ret s.I.m()-9;'),False)
 add('literal-alias',main('stack A s=.{ .x=9 }; ret consume(s)-9;'),False,prefix='type A: S_G_i32; ')
 # A generic template in the same program is not evidence against an ordinary value.
 add('unrelated-generic-scope','fn @comptime[T] ordinary(stack T unused): i32 { stack S_G_i32 s=.{ .x=9 }; ret consume(s); } '+main('ret ordinary.[i32](1)-9;'),False)
 add('ordinary-parameter-in-generic','fn @comptime[T] ordinary(stack T unused, stack S_G_i32 s): i32 { ret consume(s); } '+main('stack S_G_i32 s=.{ .x=9 }; ret ordinary.[i32](1,s)-9;'),False)
 add('generic-factory-ordinary-result',main('stack let s=make_plain.[i32](1); ret consume(s)-9;'),False,prefix='fn @comptime[T] make_plain(stack T unused): S_G_i32 { stack S_G_i32 s=.{ .x=9 }; ret s; } ')
 add('ordinary-return-within-generic','fn make_plain(void): S_G_i32 { stack S_G_i32 s=.{ .x=9 }; ret s; } fn @comptime[T] ordinary(stack T unused): i32 { stack let s=make_plain(); ret consume(s); } '+main('ret ordinary.[i32](1)-9;'),False)
 add('existing-descriptor-in-generic','fn @comptime[T] exercise(stack T unused, stack I object): i32 { ret object.m(); } fn wrapped(stack I object): i32 { ret exercise.[i32](1,object); } '+main('stack S_G_i32 s=.{ .x=9 }; ret wrapped(s)-9;'),False)
 holder='type Holder: struct { ext S_G_i32 value; }; '
 add('unknown-inferred-projection',main('stack Holder h=.{ .value=.{ .x=9 } }; stack let s=h.value; ret consume(s);'),prefix=holder)
 add('unknown-typed-projection',main('stack Holder h=.{ .value=.{ .x=9 } }; stack S_G_i32 s=h.value; ret consume(s);'),prefix=holder)
 add('known-ordinary-return-from-projection',main('stack Holder h=.{ .value=.{ .x=9 } }; stack let s=read_value(h); ret consume(s)-9;'),False,prefix=holder+'fn read_value(stack Holder h): S_G_i32 { ret h.value; } ')
 # Source declaration order is reversed independently of legacy cache spelling.
 add('generic-boxing-reversed',main('stack S.[i32] s=.{ .x=7 }; ret consume(s);'),decls=LIT+GEN)
 add('literal-boxing-reversed',main('stack S_G_i32 s=.{ .x=9 }; ret consume(s)-9;'),False,decls=LIT+GEN)
 imported='mod types; ext '+GEN.replace('type S:','type S:')+'ext '+LIT
 add('imported-generic',main('stack S.[i32] s=.{ .x=7 }; ret consume(s);'),decls='imp types; ',files={'types.sts':imported})
 add('imported-literal',main('stack S_G_i32 s=.{ .x=9 }; ret consume(s)-9;'),False,decls='imp types; ',files={'types.sts':imported})
 add('imported-generic-alias',main('stack G s=.{ .x=7 }; ret consume(s);'),decls='imp types; ',files={'types.sts':imported+'ext type G: S.[i32]; '})
 add('imported-literal-alias',main('stack A s=.{ .x=9 }; ret consume(s)-9;'),False,decls='imp types; ',files={'types.sts':imported+'ext type A: S_G_i32; '})
 # Retained expression-result metadata must survive explicitly typed destinations.
 add('generic-lambda-result',main('stack S_G_i32 s = 0 |> lam.(stack i32 unused): S.[i32] { stack S.[i32] value=.{ .x=7 }; ret value; }; ret consume(s);'))
 add('ordinary-lambda-result',main('stack S_G_i32 s = 0 |> lam.(stack i32 unused): S_G_i32 { stack S_G_i32 value=.{ .x=9 }; ret value; }; ret consume(s)-9;'),False)
 add('generic-address-qualified',main('stack S.[i32] s=.{ .x=7 }; stack S_G_i32 *rw p=&s; ret p.I.m();'))
 add('ordinary-address-qualified',main('stack S_G_i32 s=.{ .x=9 }; stack S_G_i32 *rw p=&s; ret p.I.m()-9;'),False)
 add('generic-dereference-copy',main('stack S.[i32] s=.{ .x=7 }; stack S_G_i32 *rw p=&s; stack S_G_i32 value=*p; ret consume(value);'))
 add('ordinary-dereference-copy',main('stack S_G_i32 s=.{ .x=9 }; stack S_G_i32 *rw p=&s; stack S_G_i32 value=*p; ret consume(value)-9;'),False)
 for typ,negative,name in [('S.[i32]',True,'generic-callback-result'),('S_G_i32',False,'ordinary-callback-result')]:
  maker='fn make_callback_result(void): '+typ+' { stack '+typ+' s=.{ .x=9 }; ret s; } '
  add(name,main('stack fn*(void): '+typ+' cb=make_callback_result; stack S_G_i32 s=cb(); ret consume(s);'),negative,prefix=maker)
 return cases
