"""Compile-only named trust boundaries. Acceptance is not a safety PASS."""
def gaps():
    prefix='mod gap; '
    aggregate='type I: interface { m(void): i32; }; type J: interface { m(void): i32; }; type Box: struct { ext I value; }; '
    return {
      'prior-enum-store':prefix+'type E: enum { A }; type F: enum { A }; type I: interface { m(stack E e): i32; }; fn f(stack I i): i32 { stack E value=F.A; ret i.m(value); }',
      'declared-enum-return-body':prefix+'type E: enum { A }; type F: enum { A }; type I: interface { m(stack E e): i32; }; fn fabricate(void): E { ret F.A; } fn f(stack I i): i32 { ret i.m(fabricate()); }',
      'declared-ordinary-generic-return-body':prefix+'type I: interface { m(void): i32; }; type S: @comptime[T] struct.[I] { ext T n; ext fn m(void): i32 { ret 7; } }; type S_G_i32: struct.[I] { ext i32 n; ext fn m(void): i32 { ret 9; } }; fn fabricate(void): S_G_i32 { stack S.[i32] s=.{.n=7}; ret s; } fn use(stack I i): i32 { ret i.m(); } fn f(void): i32 { stack S_G_i32 s=fabricate(); ret use(s); }',
      'default-descriptor':prefix+'type I: interface { m(void): i32; }; fn use(stack I i): i32 { ret i.m(); } fn f(void): i32 { stack I i; ret use(i); }',
      'named-aggregate-helper':prefix+aggregate+'fn helper(stack Box b): i32 { ret b.value.m(); } fn check(stack J other): i32 { stack Box b=.{.value=other}; ret helper(b); }',
      'named-aggregate-variadic':prefix+aggregate+'fn variadic(stack i32 n,...): i32 { ret n; } fn check(stack J other): i32 { stack Box b=.{.value=other}; ret variadic(0,b); }',
      'named-aggregate-thread':prefix+aggregate+'fn worker(stack Box b): i32 { ret b.value.m(); } fn check(stack J other): void { stack Box b=.{.value=other}; stack future f=thread.(worker)(b); future.drop.(f); }',
    }
