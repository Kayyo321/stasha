"""Candidate-only defined-empty cases. Execution requires independent gate."""
PREFIX='mod emptydescriptor; type I:interface{m(stack i32 n):i32;}; stack i32 side_effects=0; fn tick(void):i32{side_effects+=1;ret 1;} '
CASES={
 'direct':'fn main(void):i32{stack I value;ret value.m(tick());}',
 'alias':'type A:I;fn main(void):i32{stack A value;ret value.m(tick());}',
 'explicit-empty':'fn main(void):i32{stack I value=.{};ret value.m(tick());}',
 'mixed-field':'type H:struct{ext i32 sibling;ext I value;};fn main(void):i32{stack H h;ret h.value.m(tick());}',
 'self-field':'type H:struct{ext I value;ext fn run(void):i32{ret this.value.m(tick());}};fn main(void):i32{stack H h;ret h.run();}',
 'array-copy':'fn main(void):i32{stack I values[4];stack I value=values[3];ret value.m(tick());}',
 'mixed-ranks':'type H:struct{ext i32 sibling;ext I value;};fn main(void):i32{stack H values[2][3];stack I value=values[1][2].value;ret value.m(tick());}',
 'packed-field':'type H:@packed struct{ext u8 sibling;ext I value;};fn main(void):i32{stack H h;ret h.value.m(tick());}',
 'generic-field':'type H:@comptime[T] struct{ext T sibling;ext I value;};fn main(void):i32{stack H.[i32] h;stack I value=h.value;ret value.m(tick());}',
 'const-default':'fn main(void):i32{const I value;ret value.m(tick());}',
 'final-default':'fn main(void):i32{final I value;ret value.m(tick());}',
 'global-default':'stack I value;fn main(void):i32{ret value.m(tick());}',
 'bare-return':'fn empty(void):I{ret;}fn main(void):i32{stack I value=empty();ret value.m(tick());}',
 'partial-aggregate':'type H:struct{ext i32 sibling;ext I value;};fn main(void):i32{stack H h=.{.sibling=7};ret h.value.m(tick());}',
 'unsafe':'fn main(void):i32{stack I value;unsafe{ret value.m(tick());}}',
 'unchecked-copy':'fn main(void):i32{stack I values[2];unsafe{stack I value=values[unchecked:1];ret value.m(tick());}}',
 'loop-reentry':'''type S:struct.[I]{ext i32 n;ext fn m(stack i32 n):i32{ret this.n+n;}};
 fn check(stack I incoming):i32{for(stack i32 k=0;k<2;k++){stack I value;if(k==0){value=incoming;if(value.m(0)!=42){ret 1;}}else{ret value.m(tick());}}ret 99;}
 fn main(void):i32{stack S s=.{.n=42};ret check(s);}'''
}
CASES={name:PREFIX+body+'\n' for name,body in CASES.items()}
