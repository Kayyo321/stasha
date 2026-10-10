"""Bounded local-transfer restoration cases. Rejected inputs are compile-only."""
from i1b_cases import Case
from pathlib import Path
from dataclasses import replace
import json
import re
from i1a_origin_cases import matrix as origin_matrix
HEADER='''mod localcopies;
type I: interface { get(void): i32; bump(void): i32; };
type S: struct.[I] { ext i32 n; ext fn get(void): i32 {ret this.n;} ext fn bump(void): i32 {this.n+=1;ret this.n;} };
type T: struct {ext i32 n;};
type H: struct {ext S value;ext i32 flag;};
type Row: struct {ext S items[2];};
fn use(stack I value): i32 {ret value.get();}
'''
def matrix():
    cases=[]
    def add(name,body,positive=True,extra=''):
        cases.append(Case(name,HEADER+extra+'\nfn main(void): i32 {'+body+'}\n',positive,r'interface local transfer|interface declaration'))
    add('direct-copy-cycle','stack S a=.{.n=41}; stack S b=.{.n=7}; b=a;a=b;ret use(b)-41;')
    add('typed-field-copy','stack H h=.{.value=.{.n=41},.flag=0};stack S s=h.value;ret use(s)-41;')
    add('inferred-field-copy','stack H h=H {.value=S {.n=41},.flag=0};stack let s=h.value;ret use(s)-41;')
    add('field-write','stack H h=.{.value=.{.n=1},.flag=0};stack S seed=.{.n=41};h.value=seed;stack S s=h.value;ret use(s)-41;')
    add('whole-holder-copy','stack H h=.{.value=.{.n=1},.flag=0};stack H good=.{.value=.{.n=41},.flag=0};h=good;stack S s=h.value;ret use(s)-41;')
    add('flat-array','stack S a[2]=.{S {.n=17},S {.n=24}};stack i32 total=0;for(stack i32 i=0;i<2;i++){stack S s=a[i];total+=use(s);}ret total-41;')
    add('array-write','stack S a[2]=.{S {.n=17},S {.n=24}};stack S seed=.{.n=41};stack i32 i=1;a[i]=seed;stack let s=a[i];ret use(s)-41;')
    add('array-field','stack Row h=.{.items=.{S {.n=17},S {.n=41}}};stack S s=h.items[1];ret use(s)-41;')
    add('array-element-field','stack H a[1]=.{H {.value=.{.n=41},.flag=0}};stack S s=a[0].value;ret use(s)-41;')
    add('named-extent','const i32 N=2;stack S a[N]=.{S {.n=7},S {.n=41}};stack S s=a[1];ret use(s)-41;')
    add('inferred-extent','stack S a[]=.{S {.n=7},S {.n=41}};stack S s=a[1];ret use(s)-41;')
    add('complete-designators','stack S a[2]=.{[1]=.{.n=7},[0]=.{.n=1},[1]=.{.n=41}};stack S s=a[1];ret use(s)-41;')
    add('native-copyout','stack H h=.{.value=.{.n=41},.flag=0};if(inspect(h)!=41){ret 1;}stack S s=h.value;ret use(s)-41;',extra='fn inspect(stack H h): i32 {ret h.value.n;}')
    add('terminal-mutation','stack H h=.{.value=.{.n=41},.flag=0};stack S s=h.value;s.I.bump();ret s.n-42;')
    add('copy-before-terminal','stack H h=.{.value=.{.n=41},.flag=0};stack S s=h.value;stack S saved=s;s.I.bump();ret use(saved)-41;')
    add('shadow-initializer','stack H h=.{.value=.{.n=41},.flag=0};{stack H h=h;stack S s=h.value;if(use(s)!=41){ret 1;}}stack S s=h.value;ret use(s)-41;')
    add('exact-join','stack H h=.{.value=.{.n=41},.flag=0};stack S a=h.value;stack S b=.{.n=7};stack S s=true?a:b;ret use(s)-41;')
    add('exact-assignment-result','stack H h=.{.value=.{.n=41},.flag=0};stack S a=h.value;stack S b=.{.n=7};stack S s=(b=a);ret use(s)-41;')
    add('unrelated-matrix','stack H h=.{.value=.{.n=41},.flag=0};stack i32 a[2][2];a[0][0]=2;if(a[0][0]!=2){ret 1;}stack S s=h.value;ret use(s)-41;')
    add('unrelated-defer','stack i32 n=0;defer n=1;stack H h=.{.value=.{.n=41},.flag=0};stack S s=h.value;ret use(s)-41;')
    add('ordinary-pointer-contract','stack S s=.{.n=41};stack S *rw p=&s;ret p.I.get()-41;')
    add('helper-direct-contract','stack S s=make();ret use(s)-41;',extra='fn make(void): S {stack S s=.{.n=41};ret s;}')
    add('alias-final-copy','final Alias seed=.{.n=41};stack H h=.{.value=seed,.flag=0};const S s=h.value;ret use(s)-41;',extra='type Alias: S;')
    add('nested-field-copy','stack Outer outer=.{.inner=.{.value=.{.n=41},.flag=0}};stack S s=outer.inner.value;ret use(s)-41;',extra='type Outer: struct {ext H inner;};')
    add('unrelated-duplicate-primitives','stack i32 [_n,_n]=1,2;stack H h=.{.value=.{.n=41},.flag=0};stack S s=h.value;ret use(s)-41;')
    add('ordinary-dereference-contract','stack S s=.{.n=41};stack S *rw p=&s;stack S copy=*p;ret use(copy)-41;')
    add('ordinary-callback-contract','stack fn*(void): S cb=make;stack S s=cb();ret use(s)-41;',extra='fn make(void): S {stack S s=.{.n=41};ret s;}')
    for name,setup in [('direct-wrong-init','stack T t=.{.n=7};stack S s=t;'),('direct-wrong-write','stack S s=.{.n=41};stack T t=.{.n=7};s=t;'),('escape-then-wrong','stack S s=.{.n=41};stack S *rw p=&s;stack T t=.{.n=7};s=t;p.n=0;'),('assignment-inner-target','stack S good=.{.n=41};stack T t=.{.n=7};stack S s=(t=good);')]:add(name,setup+'ret use(s);',False)
    for name,writer in [('field-wrong','h.value=t;'),('branch-wrong','if(false){h.value=t;}'),('loop-wrong','for(stack i32 i=0;i<2;i++){h.value=t;}'),('ancestor-wrong-child','h=H {.value=t,.flag=0};'),('overwritten-wrong-child','h=H {.value=t,.value=.{.n=41},.flag=0};')]:
        add(name,'stack H h=.{.value=.{.n=41},.flag=0};stack T t=.{.n=7};'+writer+'stack S s=h.value;ret use(s);',False)
    for name,init in [('array-excess','.{S {.n=41},S {.n=7},S {.n=9}}'),('array-missing','.{S {.n=41}}'),('array-designator-excess','.{[0]=.{.n=41},[1]=.{.n=7},[2]=.{.n=9}}')]:
        add(name,'stack S a[2]='+init+';stack S s=a[0];ret use(s);',False)
    for name,escape in [('sibling-address','stack i32 *rw p=&h.flag;*p=0;'),('deferred-root','defer h.flag=1;')]:
        add(name,'stack H h=.{.value=.{.n=41},.flag=0};'+escape+'stack S s=h.value;ret use(s);',False)
    add('array-decay','stack S a[2]=.{S {.n=41},S {.n=7}};stack S *rw p=a;p[0].n=1;stack S s=a[1];ret use(s);',False)
    add('helper-child-boundary','stack H h=.{.value=make(),.flag=0};stack S s=h.value;ret use(s);',False,'fn make(void): S {stack S s=.{.n=41};ret s;}')
    add('helper-holder-boundary','stack H h=make();stack S s=h.value;ret use(s);',False,'fn make(void): H {stack H h=.{.value=.{.n=41},.flag=0};ret h;}')
    add('read-after-terminal','stack H h=.{.value=.{.n=41},.flag=0};stack S s=h.value;s.I.bump();stack S later=s;ret use(later);',False)
    add('repeat-terminal','stack H h=.{.value=.{.n=41},.flag=0};stack S s=h.value;s.I.bump();ret s.I.get();',False)
    add('loop-terminal','stack H h=.{.value=.{.n=41},.flag=0};stack S s=h.value;for(stack i32 i=0;i<2;i++){s.I.bump();}ret s.n;',False)
    add('projection-pointer','stack H h=.{.value=.{.n=41},.flag=0};stack S s=h.value;stack S *rw p=&s;ret p.I.get();',False)
    add('multi-inferred-bad-writer','stack let [s,n]=pair();stack T t=.{.n=7};s=t;ret s.I.get()+n;',False,'fn pair(void): [S,i32] {stack S s=.{.n=41};ret s,0;}')
    for fixture in sorted(Path(__file__).with_name("i1b2_regressions").glob("*.sts")):
        cases.append(Case("binding-"+fixture.stem,fixture.read_text(),False,r"interface local transfer"))
    # Fresh value-checked counterparts; historical origin fixtures remain compile-only.
    for case in origin_matrix():
        if case['name'] in ('unknown-inferred-projection','unknown-typed-projection'):
            cases.append(Case('restored-'+case['name'],case['source'].replace('ret consume(s);','ret consume(s)-9;'),True))
    cases=[replace(c,pattern=re.escape(NEGATIVE_DIAGNOSTICS[c.name])) if not c.positive else c for c in cases]
    effects=Path(__file__).with_name('i1b2_effect_regressions')
    oracles=json.loads((effects/'oracles.json').read_text())
    assert {p.stem for p in effects.glob('*.sts')} == set(oracles)
    for name,oracle in oracles.items():
        cases.append(Case('effect-'+name,(effects/(name+'.sts')).read_text(),oracle['positive'],re.escape(oracle['diagnostic'] or '')))
    return cases

# Exact owned reasons; no generic diagnostic fallback masks a changed failure.
NEGATIVE_DIAGNOSTICS = {'ancestor-wrong-child': "interface declaration: interface local transfer: 'concrete source identity does not match "
                         "the original local destination'",
 'array-decay': "interface declaration: interface local transfer: 'array-valued occurrence exposes containing storage "
                "outside an internal element/extent access'",
 'array-designator-excess': "interface declaration: interface local transfer: 'concrete-array constructor element is "
                            "outside its exact extent'",
 'array-excess': "interface declaration: interface local transfer: 'concrete-array constructor element is outside its "
                 "exact extent'",
 'array-missing': "interface declaration: interface local transfer: 'every selected concrete-array element needs an "
                  "explicit checked initializer'",
 'assignment-inner-target': "interface declaration: interface local transfer: 'assignment result destination differs "
                            "from its concrete value contract'",
 'binding-duplicate-direct-targets': "interface declaration: interface local transfer: 'duplicate multi-target "
                                     "bindings have no admitted concrete store correspondence'",
 'binding-method-result-selection': "interface declaration: interface local transfer: 'extracted result identity does "
                                    "not match the original local destination'",
 'binding-mixed-enum-shadow': "interface declaration: interface local transfer: 'concrete source identity does not "
                              "match the original local destination'",
 'binding-partial-direct-values': "interface declaration: interface local transfer: 'concrete source identity does not "
                                  "match the original local destination'",
 'binding-short-direct-values': "interface declaration: interface local transfer: 'concrete source identity does not "
                                "match the original local destination'",
 'branch-wrong': "interface declaration: interface local transfer: 'concrete source identity does not match the "
                 "original local destination'",
 'deferred-root': "interface declaration: interface local transfer: 'declaration-point deferred reference is outside "
                  "closed local proof'",
 'direct-wrong-init': "interface declaration: interface local transfer: 'concrete source identity does not match the "
                      "original local destination'",
 'direct-wrong-write': "interface declaration: interface local transfer: 'concrete source identity does not match the "
                       "original local destination'",
 'escape-then-wrong': "interface declaration: interface local transfer: 'concrete source identity does not match the "
                      "original local destination'",
 'field-wrong': "interface declaration: interface local transfer: 'concrete source identity does not match the "
                "original local destination'",
 'helper-child-boundary': "interface declaration: interface local transfer: 'ordinary parameter/result/global or "
                          "extracted value is a declared boundary, not a closed local producer'",
 'helper-holder-boundary': "interface declaration: interface local transfer: 'ordinary parameter/result/global or "
                           "extracted value is a declared boundary, not a closed local producer'",
 'loop-terminal': "interface declaration: interface local transfer: 'qualified receiver has no direct same-block "
                  "terminal frontier'",
 'loop-wrong': "interface declaration: interface local transfer: 'concrete source identity does not match the original "
               "local destination'",
 'multi-inferred-bad-writer': "interface declaration: interface local transfer: 'concrete source identity does not "
                              "match the original local destination'",
 'overwritten-wrong-child': "interface declaration: interface local transfer: 'concrete source identity does not match "
                            "the original local destination'",
 'projection-pointer': "interface declaration: unresolved/dependent concrete interface origin is unsupported for 'p'",
 'read-after-terminal': "interface declaration: interface local transfer: 'concrete source read occurs outside its "
                        "exact before-call frontier'",
 'repeat-terminal': "interface declaration: interface local transfer: 'local storage or repeated receiver effects are "
                    "outside closed proof'",
 'sibling-address': "interface declaration: interface local transfer: 'address of a local or descendant exposes the "
                    "entire root'"}
