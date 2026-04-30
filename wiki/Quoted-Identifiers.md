# Quoted Identifiers

Stasha lets you use any string as an identifier by wrapping it in `$"..."`. This is called a **quoted identifier** and allows names that would otherwise be illegal — names with spaces, punctuation, Unicode characters, or reserved keywords.

```stasha
stack i32 $"my favourite number" = 42;
stack f64 $"π"                   = 3.14159265358979;
stack i32 $"if"                  = 1;   // keyword used as a name
```

---

## Syntax

```
$"any text here"
```

- The quotes are stripped — the actual identifier name is just the text inside.
- Single quotes also work: `$'my name'`
- The name is used exactly as written; `$"foo"` and `foo` refer to the same symbol.

---

## Where it works

Quoted identifiers are valid everywhere a plain identifier can appear:

### Variable declarations

```stasha
stack i32 $"loop counter" = 0;
stack f64 $"area=π×r²"    = 3.14159 * 25.0;
let $"parsed result"       = parse(input);
```

### Function names

```stasha
int fn $"add two numbers"(stack i32 $"first", stack i32 $"second"): i32 {
    ret $"first" + $"second";
}

stack i32 sum = $"add two numbers"(10, 32);
```

### Struct fields

```stasha
type Measurement: struct {
    ext f64 $"value (cm)";
    ext f64 $"margin of error";
}

stack Measurement m = .{ .$"value (cm)" = 12.5, .$"margin of error" = 0.1 };
print.('{} ± {}\n', m.$"value (cm)", m.$"margin of error");
```

### Method names

```stasha
type Counter: struct {
    ext i32 count;
    ext fn $"increment by"(stack i32 $"how much"): void {
        this.count = this.count + $"how much";
    }
}
```

### Static constructors and methods

```stasha
fn Counter.$"from value"(stack i32 v): Counter {
    stack Counter c;
    c.count = v;
    ret c;
}

stack Counter c = Counter.$"from value"(10);
c.$"increment by"(5);
```

### Multi-assign destructuring

```stasha
stack i32 [$"min value", $"max value"] = get_range();
print.('range: {}–{}\n', $"min value", $"max value");
```

### Match arm bindings

```stasha
match result {
    Status.Ok    => { print.('ok\n'); }
    $"bad value" => { print.('bad: {}\n', $"bad value"); }
}
```

### Loop variables

```stasha
for (stack i32 $"loop index" = 0; $"loop index" < 10; $"loop index"++) {
    print.('{}\n', $"loop index");
}
```

---

## Reserved words as names

Any Stasha keyword can be used as a name this way:

```stasha
stack i32 $"if"     = 1;
stack i32 $"for"    = 2;
stack i32 $"ret"    = 3;
stack i32 $"stack"  = 4;
```

This is particularly useful when calling C functions that happen to use a Stasha keyword as a parameter name:

```stasha
lib "pthread" = pt;
pt.$"attr_init"($"attr");   // no clash with stasha keywords
```

---

## Restrictions

- **Type names cannot be quoted.** `type $"My Type": struct { ... }` is a compile error. Use a plain identifier for type declarations.
- **Module names cannot be quoted.** `mod $"my module";` is not valid.
- **No capture in lambdas.** Quoted identifier names follow the same capture rules as regular identifiers (no capturing locals in v1 lambdas).
- The name is stored verbatim in the symbol table; `$"x"` and `x` are the same symbol.

---

## Example — full showcase

```stasha
mod example;

type WeirdBox: struct {
    ext i32 $"the answer";
    ext i32 $"not the answer";

    ext fn $"do the thing"(stack i32 $"how many times"): void {
        for (stack i32 $"i" = 0; $"i" < $"how many times"; $"i"++) {
            print.('  [{}] answer={}\n', $"i", this.$"the answer");
        }
    }
}

fn WeirdBox.new(stack i32 $"answer val"): WeirdBox {
    stack WeirdBox $"result";
    $"result".$"the answer"     = $"answer val";
    $"result".$"not the answer" = 0;
    ret $"result";
}

fn main(void): void {
    stack WeirdBox $"my box" = WeirdBox.new(42);
    $"my box".$"do the thing"(3);

    stack f64 $"π"         = 3.14159265358979;
    stack f64 $"area=π×r²" = $"π" * (5.0 * 5.0);
    print.('area = {}\n', $"area=π×r²");
}
```
