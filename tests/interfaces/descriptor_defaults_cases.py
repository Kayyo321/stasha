"""Bounded descriptor defaults: category is an execution permission, never inferred."""
HEADER='mod descriptordefaults; type I: interface { m(stack i32 n): i32; }; '
IMPL='type S: struct.[I] { ext i32 value; ext fn m(stack i32 n): i32 { ret this.value+n; } }; '
BOX='type Box: struct { ext i32 sibling; ext I value; }; '
CASES={}
def add(name,category,source): CASES[name]={'category':category,'source':HEADER+source}
add('valid-later-assignment','valid',IMPL+BOX+'''fn check(stack I incoming): i32 {
 stack I value; value=incoming; stack Box box; box.sibling=8; box.value=value;
 stack I values[2]; values[1]=box.value; stack I saved=values[1]; ret saved.m(box.sibling);
} fn main(void): i32 { stack S s=.{ .value=34 }; if(check(s)!=42){ret 1;} ret 0; }''')
add('valid-loop-reentry','valid',IMPL+'''fn check(stack I incoming): i32 { stack i32 sum=0;
 for(stack i32 k=0;k<3;k++){ stack I value; value=incoming; sum+=value.m(k); }
 ret sum; } fn main(void): i32 { stack S s=.{ .value=10 }; if(check(s)!=33){ret 1;} ret 0; }''')
add('valid-self-member','valid',IMPL+'''type H: struct { ext I value; ext fn run(void): i32 {ret this.value.m(1);} };
 fn check(stack I incoming):i32 {stack H h=.{.value=incoming}; ret h.run();}
 fn main(void):i32{stack S s=.{.value=41}; if(check(s)!=42){ret 1;} ret 0;}''')
add('valid-argument-order','valid',IMPL+'''stack i32 calls=0; fn bump(void):i32{calls+=1;ret calls;}
 fn check(stack I incoming):i32{ret incoming.m(bump());}
 fn main(void):i32{stack S s=.{.value=41};if(check(s)!=42){ret 1;}if(calls!=1){ret 2;}ret 0;}''')
add('valid-nullable-controls','valid','''fn main(void):i32{stack ?i32 *r p=nil; stack ?I *r q=nil;
 stack []i32 values=nil; stack error failure=nil; if(p!=nil){ret 1;}if(q!=nil){ret 2;}
 if(len.(values)!=0){ret 3;} ret 0;}''')
add('empty-direct','empty-pending-review','fn check(void):i32{stack I value;ret value.m(1);}')
add('empty-alias','empty-pending-review','type Alias:I; fn check(void):i32{stack Alias value;ret value.m(1);}')
add('empty-field','empty-pending-review',BOX+'fn check(void):i32{stack Box box;ret box.value.m(1);}')
add('empty-self','empty-pending-review','type H:struct{ext I value;ext fn run(void):i32{ret this.value.m(1);}};fn check(void):i32{stack H h;ret h.run();}')
add('empty-array','empty-pending-review','fn check(void):i32{stack I values[3];stack I value=values[2];ret value.m(1);}')
add('layout-multirank','compile-only',BOX+'fn check(void):i32{stack Box boxes[2][3];stack I value=boxes[1][2].value;ret value.m(1);}')
add('layout-large','compile-only',BOX+'fn check(void):i32{stack Box boxes[1000000];ret 0;}')
add('layout-zero','compile-only','fn check(void):i32{stack I values[0];ret 0;}')
add('layout-packed','compile-only','type P:@packed struct{ext u8 byte;ext I value;};fn check(void):i32{stack P p;ret p.value.m(1);}')
add('layout-generic','compile-only','type H:@comptime[T] struct{ext T data;ext I value;};fn check(void):i32{stack H.[i32] h;stack I value=h.value;ret value.m(1);}')
add('layout-comptime','compile-only','type H:struct{ext i32 data; ext I value; @comptime: ext i32 marker=4;};fn check(void):i32{stack H h;ret 0;}')
add('layout-union-first','compile-only','type U:union{I value;i64 scalar;};type H:struct{ext U u;ext I value;};fn check(void):i32{stack H h;ret 0;}')
add('layout-union-last','compile-only','type U:union{i64 scalar;I value;};type H:struct{ext U u;ext I value;};fn check(void):i32{stack H h;ret 0;}')
for effect in ['atomic','volatile','tls']:
 add('excluded-root-'+effect,'compile-only',f'fn check(void):i32{{{effect} I value;ret 0;}}')
 add('excluded-field-'+effect,'source-unavailable',f'type H:struct{{{effect} ext I excluded;ext I ordinary;}};fn check(void):i32{{stack H h;ret 0;}}')
add('nil-local','reject','fn check(void):void{stack I value=nil;}')
add('nil-alias','reject','type A:I;fn check(void):void{stack A value=nil;}')
add('nil-assignment','reject','fn check(void):void{stack I value=.{};value=nil;}')
add('nil-member','reject',BOX+'fn check(void):void{stack Box box=.{};box.value=nil;}')
add('nil-self','reject','type H:struct{ext I value;ext fn clear(void):void{this.value=nil;}};')
add('nil-index','reject','fn check(void):void{stack I values[2]=.{};values[1]=nil;}')
add('nil-deref','reject','fn check(stack I *rw value):void{*value=nil;}')
add('nil-field-init','reject',BOX+'fn check(void):void{stack Box box=.{.value=nil};}')
add('nil-array-init','reject','fn check(void):void{stack I values[2]=.{nil};}')
add('nil-return','reject','fn check(void):I{ret nil;}')
add('nil-return-multi','reject','fn check(void):[I,i32]{ret nil,1;}')
add('nil-multiassign','reject','fn check(void):void{stack I [first,second]=nil,nil;}')
add('nil-unsafe','reject','fn check(void):void{stack I value=.{};unsafe{value=nil;}}')
add('deferred-array-root-nil','compile-only-deferred','fn check(void):void{stack I values[2];values=nil;}')
add('deferred-partial-rank-nil','compile-only-deferred','fn check(void):void{stack I values[2][3];values[0]=nil;}')
add('deferred-cast-nil','compile-only-deferred','fn check(void):void{stack I value=(I)nil;}')

add('deferred-field-rank-nil','compile-only-deferred','type H:struct{ext I values[2][3];};fn check(void):void{stack H h=.{.values=.{nil}};}')
add('layout-deep-unrelated','compile-only','type D0:struct{ext i32 v;};'+''.join(f'type D{i}:struct{{ext D{i-1} v;}};' for i in range(1,131))+'fn check(void):void{stack D130 value;}')
add('layout-wide-unrelated','compile-only','type W:struct{'+''.join(f'ext i32 member{i};' for i in range(4096))+'};fn check(void):void{stack W value;}')
add('layout-dag-budget','layout-budget-reject','type D0:struct{ext I value;};'+''.join(f'type D{i}:struct{{ext D{i-1} a;ext D{i-1} b;}};' for i in range(1,14))+'fn check(void):void{stack D13 value;}')
add('valid-receiver-snapshot','valid',IMPL+'''stack I target=.{};
 fn clear(void):i32{stack I empty=.{};target=empty;ret 1;}
 fn check(stack I incoming):i32{target=incoming;ret target.m(clear());}
 fn main(void):i32{stack S s=.{.value=41};if(check(s)!=42){ret 1;}ret 0;}''')
add('valid-two-argument-order','valid','''type J:interface{sum(stack i32 x,stack i32 y):i32;};
 type T:struct.[J]{ext fn sum(stack i32 x,stack i32 y):i32{ret x*10+y;}};
 stack i32 order=0;fn next(void):i32{order+=1;ret order;}
 fn check(stack J j):i32{ret j.sum(next(),next());}
 fn main(void):i32{stack T t=.{};if(check(t)!=12){ret 1;}if(order!=2){ret 2;}ret 0;}''')
add('layout-tagged-exclusion','compile-only','type E:enum{Has(stack I),None};type H:struct{ext E excluded;ext I ordinary;};fn check(void):void{stack H value;}')
add('layout-pointer-pair-shape','compile-only','type Pair:struct{stack ext void *rw a;stack ext void *rw b;};type H:struct{ext Pair unrelated;ext I ordinary;};fn check(void):void{stack H value;}')
add('layout-any-exclusion','compile-only','type H:struct{ext any.[i32,I] excluded;ext I ordinary;};fn check(void):void{stack H value;}')
_DISTINCT=''.join(f'type Scalar{j}:struct{{ext i32 value;}};' for j in range(4096))
_FIELDS=''.join(f'ext Scalar{j} member{j};' for j in range(4096))
add('layout-wide-distinct-unrelated','compile-only',_DISTINCT+'type Wide:struct{'+_FIELDS+'};fn check(void):void{stack Wide value;}')
add('layout-wide-distinct-with-leaf','compile-only',_DISTINCT+'type Wide:struct{'+_FIELDS+'ext I selected;};fn check(void):void{stack Wide value;}')
