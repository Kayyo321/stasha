"""Conventional source-only binding fixtures, reviewed alongside the I1a plan."""
def source(required='stack i32 v', actual='stack i32 v', result='i32', actual_result=None,
           prefix='', body='ret 0;', relation='I', method='m'):
    return (f'mod probe; {prefix}\ntype I: interface {{ m({required}): {result}; }};\n'
            f'type S: struct.[{relation}] {{ ext i32 x; ext fn {method}({actual}): {actual_result or result} {{ {body} }} }};\n'
            'fn main(void): i32 { ret 0; }\n')

def matrix():
    cases = []
    def add(name, text, diagnostic=None): cases.append(dict(name=name, source=text, diagnostic=diagnostic))
    mismatch = 'selected interface implementation signature mismatch'
    for t in ['bool','i8','u8','i16','u16','i32','u32','i64','u64','f32','f64']:
        add('equal-'+t, source(required=f'stack {t} v', actual=f'stack {t} v'))
    for a,b in [('i8','u8'),('i16','u16'),('i32','u32'),('i64','u64'),('i8','i16'),('i16','i32'),('i32','i64'),('f32','f64'),('bool','u8')]:
        add('param-'+a+'-'+b, source(required=f'stack {a} v', actual=f'stack {b} v'), mismatch)
        add('return-'+a+'-'+b, source(result=a,actual_result=b,body='ret 0;' if b!='bool' else 'ret false;'), mismatch)
    for name, a,b in [
        ('outer-permission','i32 *r','i32 *rw'),('inner-permission','i32 *r *rw','i32 *rw *rw'),
        ('arith','i32 *rw','i32 *'),('inner-arith','i32 *r *rw','i32 *r+ *rw'),
        ('depth','i32 *r','i32 *r *r'),('nullable','?i32 *r','i32 *r'),
        ('pointee','i32 *r','u32 *r'),('void-pointee','void *r','i32 *r')]:
        add(name+'-equal',source(required=f'stack {a} v',actual=f'stack {a} v'))
        add(name+'-mismatch',source(required=f'stack {a} v',actual=f'stack {b} v'),mismatch)
    add('pointer-domain',source(required='heap i32 *r v',actual='stack i32 *r v'),mismatch)
    add('pointer-default-domain',source(required='i32 *r v',actual='stack i32 *r v'),mismatch)
    add('scalar-default-domain',source(required='i32 v',actual='stack i32 v'))
    add('return-domain',source(result='heap i32 *r',body='ret nil;'),'storage-qualified interface returns are unsupported')
    add('enum-equal',source(prefix='type E: enum { A, B };',required='stack E v',actual='stack E v'))
    add('enum-distinct',source(prefix='type E: enum { A, B }; type F: enum { A, B };',required='stack E v',actual='stack F v'),mismatch)
    add('enum-integer',source(prefix='type E: enum { A, B };',required='stack E v',actual='stack i32 v'),mismatch)
    add('nominal-ptr-distinct',source(prefix='type A: struct { ext i32 x; }; type B: struct { ext i32 x; };',required='stack A *r v',actual='stack B *r v'),mismatch)
    add('nominal-recursive-ptr',source(prefix='type N: struct { ext N *r next; };',required='stack N *r v',actual='stack N *r v'))
    add('literal-generic-name',source(prefix='type Data_G_T: struct { ext i32 x; };',required='stack Data_G_T *r v',actual='stack Data_G_T *r v'))
    add('descriptor-equal',source(prefix='type J: interface { n(void): i32; };',required='stack J v',actual='stack J v'))
    add('descriptor-distinct',source(prefix='type J: interface { n(void): i32; }; type K: interface { n(void): i32; };',required='stack J v',actual='stack K v'),mismatch)
    add('descriptor-default-domain',source(prefix='type J: interface { n(void): i32; };',required='J v',actual='stack J v'),mismatch)
    for t in ['u8','f64']:
        prefix='type A0: '+t+'; '+''.join(f'type A{i}: A{i-1}; ' for i in range(1,7))
        add('alias-chain-'+t,source(prefix=prefix,required='stack A6 v',actual=f'stack {t} v'))
        add('alias-chain-implementation-'+t,source(prefix=prefix,required=f'stack {t} v',actual='stack A6 v'))
    add('alias-pointer-compose',source(prefix='type P: i32 *r;',required='stack P *rw v',actual='stack i32 *r *rw v'))
    add('alias-nullable-preserve',source(prefix='type P: ?i32 *r;',required='stack P v',actual='stack ?i32 *r v'))
    add('alias-inner-nullable',source(prefix='type P: ?i32 *r;',required='stack P *rw v',actual='stack P *rw v'),'inner-nullable alias composition is unsupported')
    add('alias-domain',source(prefix='type P: heap i32 *r;',required='stack P v',actual='stack P v'),'storage-qualified aliases in interface signatures are unsupported')
    add('alias-void-parameter',source(prefix='type V: void;',required='stack V v',actual='stack V v'),'void value parameter')
    add('unresolved',source(required='stack Unknown v',actual='stack Unknown v'),'unresolved nominal declaration')
    add('struct-value',source(prefix='type A: struct { ext i32 x; };',required='stack A v',actual='stack A v'),'by-value struct/union')
    add('union-value',source(prefix='type A: union { ext i32 x; ext f32 y; };',required='stack A v',actual='stack A v'),'by-value struct/union')
    add('payload-enum',source(prefix='type E: enum { Some(stack i32), None };',required='stack E v',actual='stack E v'),'payload-enum')
    for name,t in [('callback','fn*(stack i32): i32'),('closure','closure.[stack i32]: i32'),('slice','[]i32'),('callback-pointer','fn*(stack i32): i32 *r')]:
        add(name,source(required=f'stack {t} v',actual=f'stack {t} v'),'callback/closure|slice interface')
    add('missing-unused','mod probe; type I: interface { m(void): i32; }; type S: struct.[I] { ext i32 x; }; fn main(void): i32 { ret 0; }','missing instance implementation')
    add('static-lookalike','mod probe; type I: interface { m(void): i32; }; type S: struct.[I] { ext i32 x; }; fn S.m(void): i32 { ret 0; } fn main(void): i32 { ret 0; }','missing instance implementation')
    add('receiver-imitation','mod probe; type I: interface { m(void): i32; }; type S: struct.[I] { ext i32 x; }; fn S.m(stack S *rw self): i32 { ret 0; } fn main(void): i32 { ret 0; }','missing instance implementation')
    add('owner-self',source(method='S.m'))
    add('empty-unused','mod probe; type I: interface {}; type S: struct.[I] { ext i32 x; }; fn main(void): i32 { ret 0; }')
    add('equal-origins','mod probe; type A: interface { m(stack i32 a): i32; }; type B: interface { m(stack i32 b): i32; }; type C: interface.[A,B] {}; type S: struct.[C] { ext fn m(stack i32 c): i32 { ret c; } }; fn main(void): i32 { ret 0; }')
    add('conflicting-origins','mod probe; type A: interface { m(stack i32 a): i32; }; type B: interface { m(stack u32 b): i32; }; type C: interface.[A,B] {}; fn main(void): i32 { ret 0; }','distinct-origin interface signature conflict')
    add('priority-no-fallback','mod probe; type I: interface { m(stack i32 a): i32; }; type S: struct.[I] { ext fn m(stack i32 a): i32 { ret a; } ext fn I.m(stack u32 a): i32 { ret 0; } }; fn main(void): i32 { ret 0; }',mismatch)
    add('priority-shadow-callback','mod probe; type I: interface { m(stack i32 a): i32; }; type S: struct.[I] { ext fn m(stack fn*(stack i32): i32 f): i32 { ret 0; } ext fn I.m(stack i32 a): i32 { ret a; } }; fn main(void): i32 { ret 0; }')
    add('qualified-unknown',source(method='Missing.m'),'unknown interface qualifier')
    add('qualified-unrelated',source(prefix='type J: interface { m(stack i32 v): i32; };',method='J.m'),'unrelated interface qualifier')
    add('ordinary-callback','mod probe; fn call(stack fn*(stack i32): i32 f): i32 { ret f(3); } fn main(void): i32 { ret 0; }')
    add('ordinary-generic','mod probe; fn @comptime[T] identity(stack T v): T { ret v; } fn main(void): i32 { ret identity.[i32](7)-7; }')
    return cases
