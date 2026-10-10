"""Frozen leaf-factory source categories. Gate A never executes any fixture.

Gate B may execute only conventional ``valid`` sources with exact zero/empty
oracles. Reject/deferred inputs always compile to IR only, even if accepted.
"""
from dataclasses import dataclass, asdict, field
import hashlib
import re
LEVELS=(0,1,2,3)
LOCAL="interface declaration: interface local transfer: '{}'"
BOUNDARY=LOCAL.format('ordinary parameter/result/global or extracted value is a declared boundary, not a closed local producer')
MISMATCH=LOCAL.format('concrete source identity does not match the original local destination')
UNSUPPORTED=LOCAL.format('factory result requires a cleanup-free ordinary leaf with one final explicit local return')
HEADER='''mod factorycases;
type I: interface { get(void): i32; bump(void): i32; };
type S: struct.[I] { ext i32 n; ext fn get(void): i32 {ret this.n;} ext fn bump(void): i32 {this.n+=1;ret this.n;} };
type T: struct {ext i32 n;};
type H: struct {ext S value;ext i32 flag;};
fn use(stack I value): i32 {ret value.get();}
fn bump(stack I value): i32 {ret value.bump();}
'''
FACTORY='''int fn make(stack i32 n): H {
    stack S s=.{.n=n};
    stack H h=.{.value=s,.flag=5};
    ret h;
}
'''
@dataclass(frozen=True)
class Case:
    name:str
    source:str
    category:str
    boundary:str
    diagnostic_a:str|None
    span_a:tuple[int,int]|None
    diagnostic_b:str|None=None
    span_b:tuple[int,int]|None=None
    files:dict[str,str]=field(default_factory=dict)
    diagnostic_file:str='input.sts'
    expected_exit:int|None=None
    expected_stdout:str|None=None
    expected_stderr:str|None=None
    @property
    def execute(self):return self.category=='valid'

def unmark(text):
    output='';spans={};cursor=0
    for match in re.finditer(r'«([AB]+):([^»]+)»',text):
        output+=text[cursor:match.start()]
        span=(output.count('\n')+1,len(output.rsplit('\n',1)[-1])+1)
        for key in match[1]:
            if key in spans:raise ValueError('duplicate diagnostic marker')
            spans[key]=span
        output+=match[2];cursor=match.end()
    return output+text[cursor:],spans

def matrix():
    cases=[]
    def add(name,main,*,factory=FACTORY,extra='',reverse=False,category='valid',reason=None,boundary='selected leaf result',files=None):
        marked=HEADER+extra+('fn main(void): i32 {\n'+main+'\n}\n'+factory if reverse else factory+'fn main(void): i32 {\n'+main+'\n}\n')
        source,spans=unmark(marked)
        cases.append(Case(name,source,category,boundary,BOUNDARY if category!='deferred' else None,spans.get('A'),
            reason,spans.get('B'),files or {},expected_exit=0 if category=='valid' else None,
            expected_stdout='' if category=='valid' else None,expected_stderr='' if category=='valid' else None))
    for seed in (17,41):
        add('holder-copy-'+str(seed),f'stack H h=make({seed});stack H copy=h;stack let s=copy.value;\nif(use(«A:s»)!={seed}||copy.flag!=5){{ret 1;}}ret 0;')
    add('caller-before-factory','stack H h=make(17);stack H copy=h;stack let s=copy.value;ret use(«A:s»)-17;',reverse=True)
    add('zero-argument','stack H h=make();stack let s=h.value;ret use(«A:s»)-41;',factory=FACTORY.replace('stack i32 n','void').replace('.n=n','.n=41'))
    add('named-construction','stack H h=make(17);stack let s=h.value;ret use(«A:s»)-17;',factory=FACTORY.replace('s=.{','s=S {').replace('h=.{','h=H {'))
    add('aliases','stack AliasH h=make(41);final AliasH copy=h;const AliasS s=copy.value;ret use(«A:s»)-41;',extra='type AliasS: S;type AliasH: H;\n',factory=FACTORY.replace(': H {',': AliasH {').replace('stack S s','final AliasS s').replace('stack H h','stack AliasH h'))
    add('nested-path','stack Outer outer=make(17);stack Outer copy=outer;stack let s=copy.inner.value;ret use(«A:s»)-17;',extra='type Outer: struct {ext H inner;ext i32 tag;};\n',factory=FACTORY.replace(': H {',': Outer {').replace('    ret h;','    stack Outer o=.{.inner=h,.tag=9};\n    ret o;'))
    add('factory-local-writer','stack H h=make(17);stack let s=h.value;ret use(«A:s»)-41;',factory=FACTORY.replace('    stack H h','    stack S replacement=.{.n=41};s=replacement;\n    stack H h'))
    add('factory-field-writer','stack H h=make(17);stack let s=h.value;ret use(«A:s»)-41;',factory=FACTORY.replace('    ret h;','    stack S replacement=.{.n=41};h.value=replacement;\n    ret h;'))
    add('factory-root-writer','stack H h=make(17);stack let s=h.value;ret use(«A:s»)-41;',factory=FACTORY.replace('    ret h;','    stack H replacement=.{.value=.{.n=41},.flag=9};h=replacement;\n    ret h;'))
    add('caller-field-writer','stack H h=make(17);stack S replacement=.{.n=41};h.value=replacement;stack let s=h.value;ret use(«A:s»)-41;')
    add('empty-result-path','stack H h=.{.value=make(17),.flag=5};stack let s=h.value;ret use(«A:s»)-17;',factory='int fn make(stack i32 n): S {stack S s=.{.n=n};ret s;}\n')
    add('independent-boxed-copy','stack H first=make(17);stack H saved=first;stack let s=first.value;\nif(bump(«A:s»)!=18||saved.value.n!=17||first.value.n!=17||s.n!=17){ret 1;}ret use(s)-17;')
    add('repeated-call-copy','stack H first=make(17);stack H second=make(41);stack let s=first.value;stack let t=second.value;\nif(use(«A:s»)!=17||use(t)!=41||first.value.n!=17){ret 1;}ret 0;')
    add('argument-order-once','stack H h=make(tick(1),tick(7));stack let s=h.value;\nif(use(«A:s»)!=17||ticks!=17||calls!=1){ret 1;}ret 0;',
        extra='stack i32 ticks=0;stack i32 calls=0;fn tick(stack i32 n): i32 {ticks=ticks*10+n;ret n;}\n',
        factory=FACTORY.replace('stack i32 n','stack i32 a, stack i32 b').replace('.n=n','.n=a*10+b').replace('    stack S s','    calls+=1;\n    stack S s'))
    add('primitive-float-bool','stack H h=make(16.0,true);stack let s=h.value;ret use(«A:s»)-17;',factory=FACTORY.replace('stack i32 n','stack f64 x, stack bool b').replace('.n=n','.n=(i32)x+(i32)b'))
    add('primitive-trap-blocks','stack H h=make(51);stack let s=h.value;ret use(«A:s»)-17;',factory=FACTORY.replace('.n=n','.n=n/3'))
    add('scalar-helper','stack H h=make(17);stack let s=h.value;ret use(«A:s»)-41;',extra='fn scalar(stack i32 n): i32 {ret n+24;}\n',factory=FACTORY.replace('.n=n','.n=scalar(n)'))
    add('multiple-paths','stack Pair h=make(17);stack let s=h.first;stack let t=h.second;\nif(use(«A:s»)!=17||use(t)!=41){ret 1;}ret 0;',extra='type Pair: struct {ext S first;ext S second;};\n',factory='int fn make(stack i32 n): Pair {stack S a=.{.n=n};stack S b=.{.n=41};stack Pair p=.{.first=a,.second=b};ret p;}\n')
    add('two-factories','stack H a=make(17);stack H b=other(41);stack let s=a.value;stack let t=b.value;\nif(use(«A:s»)!=17||use(t)!=41){ret 1;}ret 0;',extra=FACTORY.replace('fn make','fn other'))
    add('caller-shadow','stack H h=make(17);{stack H h=h;stack let s=h.value;if(use(«A:s»)!=17){ret 1;}}stack let t=h.value;ret use(t)-17;')
    add('unrelated-generic-callback','stack i32 value=id.[i32](41);stack fn*(stack i32): i32 cb=scalar;stack H h=make(17);stack let s=h.value;\nif(use(«A:s»)!=17||cb(value)!=41){ret 1;}ret 0;',extra='fn @comptime[T] id(stack T n): T {ret n;} fn scalar(stack i32 n): i32 {ret n;}\n')
    # Two original consumers retain distinct use keys, independent of body order.
    source=HEADER+FACTORY+'fn one(void): i32 {stack H h=make(17);stack let s=h.value;ret use(«A:s»);}\nfn two(void): i32 {stack H h=make(41);stack let s=h.value;ret use(s);}\nfn main(void): i32 {if(one()!=17||two()!=41){ret 1;}ret 0;}\n'
    source,spans=unmark(source);cases.append(Case('two-consumers',source,'valid','two original consumers',BOUNDARY,spans['A'],expected_exit=0,expected_stdout='',expected_stderr=''))
    module,spans=unmark((HEADER+FACTORY+'ext fn run(void): i32 {stack H h=make(41);stack let s=h.value;ret use(«A:s»);}\n').replace('mod factorycases;','mod factorymod;'))
    cases.append(Case('source-import-owned-wrapper','mod importfactory;imp factorymod;fn main(void): i32 {ret factorymod.run()-41;}\n','valid','source-imported internal factory behind scalar wrapper',BOUNDARY,spans['A'],files={'factorymod.sts':module},diagnostic_file='factorymod.sts',expected_exit=0,expected_stdout='',expected_stderr=''))
    for name,change in [('wrong-initializer','stack T t=.{.n=n};stack S s=«B:t»;'),('wrong-local-writer','stack S s=.{.n=n};stack T t=.{.n=29};s=«B:t»;')]:
        add(name,'stack H h=make(17);stack let s=h.value;ret use(«A:s»);',factory=FACTORY.replace('stack S s=.{.n=n};',change),category='reject',reason=MISMATCH,boundary='known factory-local nominal mismatch')
    add('mismatch-over-unsupported','stack H h=make(17);stack let s=h.value;ret use(«A:s»);',factory=FACTORY.replace('stack S s=.{.n=n};','stack T t=.{.n=n};stack S s=«B:t»;stack i32 _unrelated[2]=.{1,2};'),category='reject',reason=MISMATCH,boundary='known mismatch dominates unsupported sibling storage')
    add('mismatch-over-frontier','stack H h=make(17);stack let s=h.value;ret use(«A:s»);',factory=FACTORY.replace('stack S s=.{.n=n};','stack T t=.{.n=n};stack S s=«B:t»;s.I.get();'),category='reject',reason=MISMATCH,boundary='known mismatch dominates receiver frontier')
    add('generic-stored-history','stack H h=make(17);stack let s=h.value;ret use(«A:s»);',extra='type G: @comptime[T] struct {ext T n;};\n',factory=FACTORY.replace('stack S s=.{.n=n};','stack G.[i32] g=.{.n=n};stack S s=«B:g»;'),category='reject',reason=MISMATCH,boundary='same-annotation generic stored history is not a seed')
    # Cleanup is a whole-body whitelist. These forms are compile-only even
    # when an unrelated selected path was built from ordinary local values.
    for name,extra,statement in [
        ('generic-local', 'type G: @comptime[T] struct {ext T n;};\n', 'stack G.[i32] _g=.{.n=3};'),
        ('generic-destructor', 'type G: @comptime[T] struct {ext T n;ext fn rem(void): void {this.n=0;}};\n', 'stack G.[i32] _g=.{.n=3};'),
        ('ordinary-destructor', 'type D: struct {ext i32 n;ext fn rem(void): void {this.n=0;}};\n', 'stack D _d=.{.n=3};'),
        ('nested-destructor', 'type D: struct {ext i32 n;ext fn rem(void): void {this.n=0;}};type Box: struct {ext D d;};\n', 'stack Box _box=.{.d=.{.n=3}};'),
        ('enum-local', 'type E: enum {A,B};\n', 'stack E _e;'),
        ('array-local', '', 'stack i32 _a[2]=.{1,2};'),
        ('callback-local', 'fn scalar(void): i32 {ret 3;}\n', 'stack fn*(void): i32 _cb=scalar;'),
        ('discarded-aggregate-call', 'fn extra(stack i32 n): S {stack S s=.{.n=n};ret s;}\n', 'extra(n);'),
        ('nested-block-cleanup', 'type D: struct {ext i32 n;ext fn rem(void): void {this.n=0;}};\n', '{stack D _d=.{.n=3};}'),
        ('target-elided-cleanup', 'type D: struct {ext i32 n;ext fn rem(void): void {this.n=0;}};\n', 'comptime_if os == "windows" {stack D _d=.{.n=3};}'),
        ('defer-body', '', 'defer n=1;'),
        ('relevant-address', '', 'stack i32 *rw _p=&h.flag;'),
    ]:
        fac=FACTORY.replace('    ret h;', '    '+statement+'\n«B:ret» h;')
        if name=='relevant-address':fac=fac.replace('«B:ret»','ret').replace('stack H h=','stack H «B:h»=')
        add('unsupported-'+name,'stack H h=make(17);stack let s=h.value;ret use(«A:s»);',factory=fac,extra=extra,category='reject',reason=UNSUPPORTED,boundary='unclassified or cleanup-bearing whole factory body')
        # Exactly the same failed request remains silent behind an unemitted sink.
        fac=fac.replace('«B:ret»','ret').replace('«B:h»','h')
        add('dead-'+name,'stack H _h=make(17);ret 0;stack let s=_h.value;ret use(s);',factory=fac,extra=extra,category='deferred',boundary='unconsumed cleanup/unsupported request stays silent')
    add('public-factory','stack H h=make(17);stack let s=h.value;ret use(«A:s»);',factory=FACTORY.replace('int fn','fn').replace('    ret h;','«B:ret» h;'),category='reject',reason=UNSUPPORTED,boundary='public/interposable source result is not admitted')
    add('child-factory','stack H h=make(17);stack let s=h.value;ret use(«A:s»);',extra='int fn child(stack i32 n): S {stack S s=.{.n=n};ret s;}\n',factory=FACTORY.replace('stack S s=.{.n=n};','stack S s=«B:child»(n);'),category='reject',reason=UNSUPPORTED,boundary='no nominal child-factory proof chains')
    for name,body,fac in [
        ('dead-projected-sink','stack H _h=make(17);ret 0;stack let s=_h.value;ret use(s);',FACTORY.replace('stack H h=', 'stack H _h=').replace('    ret h;','    ret;')),
        ('dead-mismatch-sink','stack H _h=make(17);ret 0;stack let s=_h.value;ret use(s);',FACTORY.replace('stack S s=.{.n=n};','stack T t=.{.n=n};stack S s=t;')),
        ('call-after-return','ret 0;stack H h=make(17);stack let s=h.value;ret use(s);',FACTORY),
    ]:add(name,body,factory=fac,category='deferred',boundary='unemitted source sink never consumes new proof')
    add('legacy-direct-s-history','stack S s=make(17);ret use(s);',factory='int fn make(stack i32 n): S {stack T t=.{.n=n};stack S s=t;ret s;}\n',category='deferred',boundary='same-annotation direct-S history remains an unclosed legacy boundary')
    return cases

def inventory(cases):
    result=[]
    for case in cases:
        row=asdict(case);row['source_sha256']=hashlib.sha256(row.pop('source').encode()).hexdigest()
        row['files']={name:hashlib.sha256(text.encode()).hexdigest() for name,text in sorted(case.files.items())}
        row['execute_gate_a']=False;row['execute_gate_b']=case.execute;result.append(row)
    return result
