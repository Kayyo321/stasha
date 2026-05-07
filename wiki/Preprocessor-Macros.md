# Preprocessor Macros

Stasha has a token preprocessor for reusable syntax fragments. Macros expand before parsing, so the parser sees ordinary Stasha tokens after expansion.

---

## Function-Like Macros

Declare a macro with `macro fn name!` and invoke it with `name.!()`:

```stasha
int macro fn say_hello! {
    () => {
        print.('hello\n');
    };
}

fn main(void): i32 {
    say_hello.!();
    ret 0;
}
```

Macro parameters use `@name` placeholders:

```stasha
ext macro fn repeat! {
    (@msg, @count) => {
        for (i32 i = 0; i < @count; i++) {
            print.(@msg);
        }
    };
}

repeat.!('again\n', 3);
```

`int` macros are visible only in the current module. `ext` macros can be imported by other modules.

---

## Token Aliases

`macro let` creates a token alias:

```stasha
int macro let integer! = i32;

const integer.! n = 10;  // expands to: const i32 n = 10;
```

This is useful for small syntactic aliases, not for values that need type checking. For values, prefer `const`.

---

## Importing Macros

External macros are imported through normal module imports:

```stasha
imp ex_preproc = pp;

fn main(void): i32 {
    pp.repeat.!('from another file\n', 2);
    ret 0;
}
```

See [`examples/ex_preproc.sts`](../examples/ex_preproc.sts) and [`examples/ex_preproc_other.sts`](../examples/ex_preproc_other.sts).

---

## Compile-Time Macro Loops

Macro patterns can capture variadic arguments with `...@args`. Macro bodies can then use `@foreach` to repeat expansion over those arguments:

```stasha
int macro write_out! {
    (...@args) => {
        @foreach arg : args {
            print.('{} ', arg);
        }
        print.('\n');
    }
}

write_out.!('hi', 110, 3.14);
```

The loop runs during preprocessing. It emits tokens into the expanded source stream; it does not create a runtime loop unless the emitted tokens contain one.

---

## Notes

- Macro names conventionally end in `!`, and invocations use `.!`.
- Macro expansion is token-based. Keep generated code small and clear.
- Prefer functions, generics, and `@comptime if` when ordinary language features are expressive enough.
