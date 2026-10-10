"""I1b.1 source-call matrix. Every negative and deferred case is compile-only."""
from dataclasses import dataclass, field

@dataclass(frozen=True)
class Case:
    name: str
    source: str
    positive: bool = True
    pattern: str = r'interface source argument|interface declaration'
    files: dict = field(default_factory=dict)


def slot(formal, actual, setup='', *, prelude='', result='i32', body='ret 41;', expected='41', qualified=False, params=''):
    declaration=f'type I: interface {{ m({formal} value): {result}; }}; type S: struct.[I] {{ ext fn m({formal} value): {result} {{ {body} }} }}; '
    if qualified:
        return f'mod sourcecall; {prelude} {declaration} fn main(void): i32 {{ stack S object=.{{}}; {setup} if (object.I.m({actual}) != {expected}) {{ ret 1; }} ret 0; }}'
    return f'mod sourcecall; {prelude} {declaration} fn check(stack I object{params}): i32 {{ {setup} if (object.m({actual}) != {expected}) {{ ret 1; }} ret 0; }} fn main(void): i32 {{ stack S object=.{{}}; ret check(object); }}'


def matrix():
    out=[]
    def add(name,source,positive=True):out.append(Case(name,source,positive))
    for qualified in (False,True):
        route='qualified' if qualified else 'dynamic'
        for typ,value in [('i8','-120'),('u8','240'),('i16','-30000'),('u16','60000'),('i32','-2000000000'),('u32','4000000000'),('i64','-8000000000000000000'),('u64','16000000000000000000'),('f32','1.25'),('f64','3.5')]:
            add(f'{route}-primitive-{typ}',slot('stack '+typ,'v',f'stack {typ} v={value};',body='ret value;',result=typ,expected=value,qualified=qualified))
        for name,formal,actual,setup,prelude,body in [
            ('enum-alias','stack E','v','stack EA v=E.B;','type E: enum { A,B }; type EA: E;','if (value==E.B) { ret 41; } ret 9;'),
            ('enum-field','stack E','h.v','stack H h=.{ .v=E.B };','type E: enum { A,B }; type H: struct { ext E v; };','if (value==E.B) { ret 41; } ret 9;'),
            ('enum-array','stack E','v[1]','stack E v[2]=.{ E.A,E.B };','type E: enum { A,B };','if (value==E.B) { ret 41; } ret 9;'),
            ('enum-result','stack E','pick()','','type E: enum { A,B }; fn pick(void): E { ret E.B; }','if (value==E.B) { ret 41; } ret 9;'),
            ('outer-attenuation','stack i32 *r','p','stack i32 v=41; stack i32 *rw p=&v;','','ret *value;'),
            ('fresh-local-arithmetic','stack i32 *rw+','&v','stack i32 v=41;','','ret value[0];'),
            ('fresh-array-arithmetic','stack i32 *rw+','&v[0]','stack i32 v[2]=.{ 41,9 };','','ret value[0];'),
            ('array-pointer-element','stack i32 *r','v[0]','stack i32 n=41; stack i32 *r p=&n; stack i32 *r v[1]=.{ p };','','ret value[0];'),
            ('pointer-field-domain','stack i32 *r','h.p','stack i32 n=41; stack H h=.{ .p=&n };','type H: struct { stack ext i32 *r p; };','ret value[0];'),
            ('nullable-widen','stack ?i32 *r','p','stack i32 n=41; stack i32 *rw p=&n;','','ret 41;'),
            ('nullable-default-nil','?i32 *r','nil','','','ret 41;'),
            ('pointer-layer-exact','stack i32 *r *r','pp','stack i32 n=41; stack i32 *r p=&n; stack i32 *r *r pp=&p;','','ret **value;'),
            ('primitive-cast','stack i32','(i32)v','stack i64 v=41;','','ret value;'),
            ('primitive-conditional','stack i32','true ? v : w','stack i32 v=41; stack i32 w=9;','','ret value;'),
            ('primitive-assignment','stack i32','(v=41)','stack i32 v=9;','','ret value;'),
            ('primitive-compound','stack i32','(v+=1)','stack i32 v=40;','','ret value;'),
            ('primitive-postinc','stack i32','v++','stack i32 v=41;','','ret value;'),
        ]:add(f'{route}-{name}',slot(formal,actual,setup,prelude=prelude,body=body,qualified=qualified))
        common='type E: enum { A,B }; type F: enum { A,B }; type X: struct { ext i32 n; }; type Y: struct { ext i32 n; };'
        for name,formal,actual,setup in [
            ('enum-other','stack E','f','stack F f=F.B;'),('enum-integer','stack E','1',''),
            ('enum-to-integer','stack i32','e','stack E e=E.B;'),('enum-arithmetic','stack i32','e+0','stack E e=E.B;'),
            ('enum-cast-deref','stack E','*(E *r)&f','stack F f=F.B;'),
            ('pointer-pointee','stack X *r','&y','stack Y y=.{};'),
            ('pointer-to-integer','stack i32','&v','stack i32 v=41;'),
            ('integer-to-pointer','stack i32 *r','41',''),
            ('permission-widen','stack i32 *rw','p','stack i32 v=41; stack i32 *r p=&v;'),
            ('arithmetic-launder','stack i32 *r+','&*p','stack i32 v=41; stack i32 *r p=&v;'),
            ('depth-mismatch','stack i32 *r','&p','stack i32 v=41; stack i32 *r p=&v;'),
            ('inner-layer-change','stack i32 *r *r','pp','stack i32 v=41; stack i32 *rw p=&v; stack i32 *rw *r pp=&p;'),
            ('nullable-narrow','stack i32 *r','p','stack i32 v=41; stack ?i32 *r p=&v;'),
            ('nil-nonnullable','i32 *r','nil',''),('nil-known-domain','stack ?i32 *r','nil',''),
            ('domain-mismatch','heap i32 *r','&v','stack i32 v=41;'),
            ('slot-domain','heap i32 *r *r','&p','heap i32 *r p=nil;'),
            ('cast-launder','stack X *r','(X *r)&y','stack Y y=.{};'),
            ('assignment-launder','stack E','(e=f)','stack E e=E.A; stack F f=F.B;'),
            ('literal-range','stack u8','256',''),
            ('signedness','stack u32','v','stack i32 v=41;'),
        ]:add(f'{route}-reject-{name}',slot(formal,actual,setup,prelude=common,qualified=qualified),False)
    descriptor=('mod descriptors; type I: interface { id(void): i32; }; type J: interface { id(void): i32; }; '
        'type S: struct.[I,J] { ext i32 n; ext fn id(void): i32 { ret this.n; } }; '
        'fn use(stack I value): i32 { ret value.id(); } fn reflect(I value): I { ret value; } ')
    add('native-exact-descriptor-result',descriptor+'fn check(I value): i32 { ret use(reflect(value))-41; } fn main(void): i32 { stack S s=.{ .n=41 }; ret check(s); }')
    add('native-readonly-descriptor',descriptor+'fn check(I value): i32 { const I copy=value; ret use(copy)-41; } fn main(void): i32 { stack S s=.{ .n=41 }; ret check(s); }')
    add('native-function-pointer',descriptor+'fn check(I value): i32 { stack fn*(stack I): i32 f=use; ret f(value)-41; } fn main(void): i32 { stack S s=.{ .n=41 }; ret check(s); }')
    add('native-generic-independent',descriptor+'fn @comptime[T] forward(stack T unused, I value): i32 { ret use(value); } fn check(I value): i32 { ret forward.[i32](1,value)-41; } fn main(void): i32 { stack S s=.{ .n=41 }; ret check(s); }')
    add('reject-native-wrong-descriptor',descriptor+'fn check(J other): i32 { ret use(other); } fn main(void): i32 { ret 0; }',False)
    add('reject-native-forged-descriptor',descriptor+'fn check(J other): i32 { ret use(*(I *r)&other); } fn main(void): i32 { ret 0; }',False)
    add('reject-inferred-variadic',descriptor+'fn vararg(...): i32 { ret 0; } fn check(I value): i32 { stack let inferred=value; ret vararg(inferred); } fn main(void): i32 { ret 0; }',False)
    add('reject-thread-carrier',descriptor+'fn check(I value): i32 { stack future f=thread.(use)(value); future.drop.(f); ret 0; } fn main(void): i32 { ret 0; }',False)
    for expr in ['*p + 0','(i32)*p','true ? (*p) : 41']:
        name={'*p + 0':'binary','(i32)*p':'cast','true ? (*p) : 41':'conditional'}[expr]
        add('reject-unreadable-'+name,'mod unreadable; type I: interface { m(stack i32 value): i32; }; fn check(stack I object, stack i32 *w p): i32 { unsafe { ret object.m('+expr+'); } } fn main(void): i32 { ret 0; }',False)
    adapter=descriptor+"""
    type Helper: struct { ext i32 n;
      ext fn receive(stack I value): i32 { ret value.id(); }
      ext fn sibling(stack I value): i32 { ret receive(value); }
      ext fn selfcopy(stack I value): i32 { ret this.receive(value); }
    };
    type Holder: struct { ext Helper helper; ext fn pass(stack I value): i32 { ret this.helper.receive(value); } };
    fn Helper.staticcopy(stack I value): i32 { ret value.id(); }
    fn Helper.new(stack I value): Helper { stack Helper h=.{ .n=value.id() }; ret h; }
    """
    for route,expr,setup in [('instance','h.receive(V)','stack Helper h=.{.n=0};'),('sibling','h.sibling(V)','stack Helper h=.{.n=0};'),('self','h.selfcopy(V)','stack Helper h=.{.n=0};'),('member','holder.pass(V)','stack Holder holder=.{.helper=.{.n=0}};'),('static','Helper.staticcopy(V)',''),('constructor','made.n','stack Helper made=Helper.(V);')]:
        for wrong in [False,True]:
            v='other' if wrong else 'value';expression=expr.replace('V',v);initial=setup.replace('V',v)
            source=adapter+f'fn check(I value, J other): i32 {{ {initial} ret {expression}-41; }} fn main(void): i32 {{ stack S s=.{{.n=41}}; ret check(s,s); }}'
            add(('reject-' if wrong else '')+'adapter-'+route,source,not wrong)
    special={
      'literal-lambda':'fn check(I value): i32 { ret value |> lam.(stack I object): i32 { ret object.id(); }; }',
      'closure':'fn check(I value): i32 { stack i32 _offset=1; stack closure.[stack I]: i32 cb=lam.(stack I object): i32 { .|_offset| ret object.id()+_offset; }; ret cb(value); }',
      'async':'async fn work(stack I object): i32 { ret object.id(); } fn check(I value): i32 { stack future f=async.(work)(value); future.drop.(f); ret 0; }',
      'error':'fn risky(stack I object): error { ret nil; } fn check(I value): error { risky.?(value); ret nil; }',
      'c':'@[[abi: c]] ext fn foreign(stack I object): i32; fn check(I value): i32 { ret foreign(value); }',
      'typed-variadic':'fn variadic(...typed): i32 { ret 0; } fn check(I value): i32 { ret variadic(value); }',
    }
    for name,body in special.items():
        out.append(Case('reject-carrier-'+name,'mod carrier; type I: interface { id(void): i32; }; '+body+' fn main(void): i32 { ret 0; }',False,pattern=r'C ABI: unsupported aggregate member representation|interface-bearing C' if name=='c' else r'interface source argument.*(?:carrier|contract)'))
    for reverse in [False,True]:
        actual='choose() ? cached : (side(value))' if reverse else 'choose() ? side(value) : cached'
        count=1 if reverse else 12
        add('same-I-conditional-'+str(int(reverse)),descriptor+'stack i32 count=0; fn choose(void): bool { count=count*10+1; ret true; } fn side(I v): I { count=count*10+2; ret v; } '+f'fn check(I value): i32 {{ stack I cached=value; if(use({actual})!=41||count!={count}) {{ ret 1; }} ret 0; }} fn main(void): i32 {{ stack S s=.{{.n=41}}; ret check(s); }}')
    for reverse in [False,True]:
        add('same-E-conditional-'+str(int(reverse)),slot('stack E','false ? value : E.A' if reverse else 'true ? E.A : value','stack E value=E.B;',prelude='type E: enum { A,B };',body='ret value;',result='E',expected='E.A'))
    api='mod api; ext type I: interface { id(void): i32; }; ext type J: interface { id(void): i32; }; ext fn take(stack I v): i32 { ret v.id(); }'
    for route,callee in [('module','api.take'),('colon','api:take')]:
        for wrong in [False,True]:
            t='J' if wrong else 'I'
            source=f'mod caller; imp api; type S: struct.[I,J] {{ ext fn id(void): i32 {{ ret 41; }} }}; fn check({t} v): i32 {{ ret {callee}(v)-41; }} fn main(void): i32 {{ stack S s=.{{}}; ret check(s); }}'
            out.append(Case(('reject-' if wrong else '')+'adapter-'+route,source,not wrong,files={'api.sts':api}))
    # Merely declaring an unused interface must not change unrelated calls.
    unrelated = {
      'callback-field': 'type Callback: fn*(stack i32): i32; type H: struct { ext Callback callback; }; fn identity(stack i32 x): i32 { ret x; } fn consume(stack Callback cb): i32 { ret cb(41); } fn main(void): i32 { stack H h=.{ .callback=identity }; ret consume(h.callback)-41; }',
      'callback-array-field': 'type Callback: fn*(stack i32): i32; type H: struct { ext Callback callbacks[1]; }; fn identity(stack i32 x): i32 { ret x; } fn consume(stack Callback cb): i32 { ret cb(41); } fn main(void): i32 { stack H h=.{ .callbacks=.{identity} }; ret consume(h.callbacks[0])-41; }',
      'error-field': 'type H: struct { ext error value; }; fn consume(stack error value): i32 { ret 0; } fn main(void): i32 { stack H h=.{ .value=nil }; ret consume(h.value); }',
      'array-slice-field': 'type H: struct { ext i32 values[2]; }; fn consume(stack []i32 values): i32 { ret values[0]+values[1]; } fn main(void): i32 { stack H h=.{ .values=.{17,24} }; ret consume(h.values[:])-41; }',
    }
    for name, body in unrelated.items():
        for unused_interface in [False,True]:
            add('ordinary-'+name+'-'+str(int(unused_interface)), 'mod ordinary; '+('type I: interface { id(void): i32; }; ' if unused_interface else '')+body)
    add('native-nested-generic-slice-selection', 'mod nested; type View: []i32; type I: interface { id(void): i32; }; type S: struct.[I] { ext fn id(void): i32 { ret 41; } }; fn @comptime[T] view(stack []i32 values): View { ret values; } fn pair(stack []i32 values, I object): i32 { ret values[0]+object.id(); } fn check(I object): i32 { stack i32 values[1]=.{1}; ret pair(view.[i32](values[:])[:],object)-42; } fn main(void): i32 { stack S s=.{}; ret check(s); }')
    return out
