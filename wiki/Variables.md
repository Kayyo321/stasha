# Variables

Variables in Stasha are explicit about how and where data lives. This makes memory behavior predictable and safe.

---

## Declaration Syntax

```
[storage_qualifier] type name = value;
```

The storage qualifier is optional for local non-pointer variables (the compiler defaults to `stack`), but being explicit is good practice and required in some contexts.

```stasha
stack i32 x = 42;          // explicit
i32 y = 100;               // implicit stack (same thing)
final f64 gravity = 9.81;
const i32 MAX = 256;
```

---

## Storage Qualifiers

### `stack` — Stack Allocated

The variable lives on the call stack. Fast, automatically freed when the scope exits. This is the default.

```stasha
stack i32 count = 0;
stack f64 radius = 3.14;
stack bool active = true;
```

### `heap` — Heap Pointer

The variable holds a **pointer** to heap-allocated memory. You own the allocation and must call `rem.()` to free it (or use `defer`).

```stasha
heap i32 *rw buf = new.(sizeof.(i32) * 10);
defer rem.(buf);
```

Note: `heap` on a non-pointer variable (like `heap i32 x`) is a compile error. Use `heap []T` slices or pointer types.

### `const` — Compile-Time Constant

Set once at compile time, immutable forever. No writable pointer can be derived from a `const`.

```stasha
const i32 MAX_USERS = 1024;
const f64 PI = 3.14159265358979;
const bool DEBUG = false;
```

Use `const` for any value that doesn't change. The compiler can inline and optimize these aggressively.

### `final` — Runtime Immutable

Initialized at runtime, but cannot be reassigned after that. Like `const val` in Swift or `final` in Java.

```stasha
final i32 argc = get_argc();   // set from runtime call
final f64 scale = config.get_scale();
// scale = 2.0;  // ERROR: cannot assign to final
```

Only a read-only pointer (`*r`) can be derived from a `final`.

### `atomic` — Thread-Safe

An atomic variable allows safe concurrent reads and writes without a mutex. Useful for shared counters and flags.

```stasha
int atomic i32 request_count = 0;   // shared across threads

// From any thread:
request_count = request_count + 1;  // atomic load + store
```

`atomic` variables are allowed as module-level globals.

### `volatile` — No Optimization

Prevents the compiler from optimizing away loads and stores. Essential for memory-mapped I/O, spin-wait loops, or interfacing with hardware registers.

```stasha
volatile i32 status_reg = 0;

// Even if the compiler thinks status_reg is "never read elsewhere",
// it will still emit the load on every iteration:
while (status_reg == 0) { }   // spin-wait
```

### `tls` — Thread-Local Storage

Each thread gets its own independent copy of the variable. Changes in one thread don't affect other threads.

```stasha
ext tls i32 thread_id = 0;
ext tls i32 error_code = 0;
```

---

## Multiple Variables

Declare several variables of the same type in one statement:

```stasha
stack i32 a = 1, b = 2, c = 3;
stack f64 x = 0.0, y = 0.0, z = 1.0;
```

---

## Type Inference with `let`

`let` infers the type from the right-hand side. Useful with multi-return functions:

```stasha
let result = compute();                    // infer single value
let [x, y] = get_coords();                // multi-return destructure
let [value, err] = parse_int(str);        // common pattern
```

If you don't care about one of the returned values, use `_`:

```stasha
let [_, err] = risky_operation();
if err != nil { handle_error(err); }
```

---

## Global Variables

Module-level variables use static storage:

```stasha
int i32 counter = 0;              // private to this module
ext i32 public_count = 100;       // exported (callable from other modules)
int const i32 MAX = 256;          // private constant
int atomic i32 shared = 0;        // private atomic global
```

Global variables are zero-initialized by default.

---

## Pointer Variables

Pointer variables need both a storage domain and a permission:

```stasha
stack i32 *rw p = &local_var;          // stack pointer (points at stack memory)
heap  u8  *rw buf = new.(256);          // heap pointer (points at heap memory)
heap  i32 *r  readonly = new.(4);       // read-only heap pointer
heap  i32 *rw+ arith   = new.(40);      // read-write + arithmetic
```

The storage qualifier describes **where the pointed-to data lives**, not where the pointer variable itself lives.

See [Memory Management](Memory-Management) and [Safety System](Safety-System) for the full rules.

---

## Fixed-Size Arrays

```stasha
i32 arr[8];                     // 8 ints, zero-initialized
i32 arr[8] = .{1, 2, 3, 4};    // partial init, rest zeroed
i32 arr[] = .{1, 2, 3};        // size inferred from initializer (3)
```

Access the length via the built-in `.len` field:

```stasha
i32 buf[10];
print.('{}\n', buf.len);   // 10
```

---

## Compound Initializers

Stasha has expressive initializer syntax for arrays and structs:

```stasha
// Designated index
i32 c[3] = .{[1] = 5};         // [0, 5, 0]

// Ranges
i32 r1[] = .{0..5};            // [0, 1, 2, 3, 4]
i32 r2[] = .{0..=5};           // [0, 1, 2, 3, 4, 5]
i32 r3[] = .{0..10:2};         // [0, 2, 4, 6, 8] — step of 2

// Spread
i32 base[] = .{1, 2, 3};
i32 ext[]  = .{..base, 4, 5};  // [1, 2, 3, 4, 5]

// Struct initializer
type Player: struct { i32 hp; i32 mp; }
Player p = .{.hp = 100, .mp = 50};

// Struct spread (copy and override)
Player p2 = .{..p, .hp = 200};  // copies p, overrides hp

// Zero init
Player empty = .{};             // all fields zeroed
```

---

## Common Mistakes

**Forgetting to free heap memory:**
```stasha
heap i32 *rw buf = new.(100);   // OK
// ... use buf ...
// forgot rem.(buf) — memory leak!
```

Use `defer` immediately after allocation:
```stasha
heap i32 *rw buf = new.(100);
defer rem.(buf);                // runs at scope exit no matter what
```

**Using `heap` on a non-pointer type:**
```stasha
heap i32 x = 5;   // ERROR: non-pointer heap variable
```

Fix:
```stasha
heap i32 *rw x = new.(sizeof.(i32));
*.(x) = 5;
```

**Trying to assign to a `const`:**
```stasha
const i32 N = 10;
N = 20;   // ERROR: cannot assign to const
```

---

## Quoted Identifiers

Variable names can contain spaces, keywords, or special characters using the `$"..."` syntax:

```stasha
stack i32 $"my count" = 0;
stack f64 $"π"        = 3.14159;
stack i32 $"if"       = 1;   // keyword as a name
```

See [Quoted Identifiers](Quoted-Identifiers) for the full reference.
