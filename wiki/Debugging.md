# Debugging

Stasha supports source-level debugging via DWARF debug info and standard debuggers.

---

## Debug Builds

Add `-g` to emit debug symbols:

```bash
stasha myapp.sts -g -o bin/myapp_debug
```

On macOS, this also generates a `.dSYM` bundle:
```bash
bin/myapp_debug.dSYM/
```

For maximum debuggability, combine `-g` with no optimization:
```bash
stasha myapp.sts -g -o=0 -o bin/debug/myapp
```

---

## Using `lldb` (macOS)

```bash
lldb bin/myapp_debug
(lldb) break set -n main        # breakpoint on main
(lldb) run                      # start the program
(lldb) next                     # step over
(lldb) step                     # step into
(lldb) finish                   # step out
(lldb) print x                  # inspect variable
(lldb) frame variable           # all locals
(lldb) continue                 # resume
(lldb) quit
```

### Breakpoints

```bash
(lldb) break set -f myfile.sts -l 42     # line breakpoint
(lldb) break set -n my_function          # function breakpoint
(lldb) break list                        # list breakpoints
(lldb) break delete 1                   # delete breakpoint 1
```

### Inspecting Variables

```bash
(lldb) print x                  # print variable x
(lldb) print *ptr               # dereference pointer
(lldb) print arr[0]             # array element
(lldb) frame variable           # all locals in current frame
(lldb) bt                       # backtrace
```

---

## Using `gdb` (Linux)

```bash
gdb bin/myapp_debug
(gdb) break main
(gdb) run
(gdb) next
(gdb) step
(gdb) print x
(gdb) info locals
(gdb) backtrace
(gdb) continue
(gdb) quit
```

---

## Print Debugging

The `print.()` built-in is always available and needs no import:

```stasha
print.('debug: x = {}\n', x);
print.('debug: ptr = {:x}\n', (u64)ptr);
print.('debug: arr = ');
for (i32 i = 0; i < n; i++) {
    print.('{} ', arr[i]);
}
print.('\n');
```

Print to stderr with `print.error.()`:
```stasha
print.error.('ERROR: {}\n', message);
```

---

## Compile-Time Assertions

Catch bugs at compile time with `comptime_assert.()`:

```stasha
comptime_assert.(sizeof.(Header) == 16);
comptime_assert.(sizeof.(i32) == 4);
```

If the assertion fails, the compile fails with a clear error — before you ever run the code.

---

## Runtime Assertions (Bounds Checks)

Slice indexing automatically bounds-checks and calls `llvm.trap` on violation:

```stasha
heap []i32 s = make.([]i32, 5);
s[10] = 0;   // runtime abort: index 10 >= len 5
```

The program terminates with a SIGILL/SIGABRT and you get a core dump for debugging.

---

## Sanitizers

Compile with LLVM sanitizers for deeper debugging:

```bash
# AddressSanitizer — detect heap/stack overflows, use-after-free
stasha myapp.sts -g -o=0 -o myapp_asan
# Then run with asan environment variables:
ASAN_OPTIONS=detect_leaks=1 ./myapp_asan
```

*(Sanitizer support depends on your LLVM build configuration.)*

---

## Debug Patterns

### Conditional Debug Output

```stasha
const bool DEBUG = false;   // flip to true for debug builds

fn debug_print(stack i8 *r msg): void {
    comptime_if DEBUG {
        print.error.('[DEBUG] {}\n', msg);
    }
}
```

### Struct Print Method

Add a print method to your structs:

```stasha
type Point: struct {
    ext i32 x, y;

    ext fn print(void): void {
        print.('Point({}, {})\n', this.x, this.y);
    }
}

Point p = .{ .x = 3, .y = 4 };
p.print();   // Point(3, 4)
```

### Value Tracing

```stasha
fn trace_i32(stack i8 *r name, stack i32 val): i32 {
    print.('[TRACE] {} = {}\n', name, val);
    ret val;
}

// Wrap expressions:
stack i32 result = trace_i32("result", compute());
```

---

## Common Crash Scenarios

| Symptom | Likely Cause |
|---------|-------------|
| `SIGABRT` / SIGILL | Bounds check violation (slice OOB) |
| Segfault on nil | Dereferencing nil pointer |
| Segfault in freed memory | Use-after-free (use AddressSanitizer) |
| Wrong values | Stale pointer / aliasing bug |
| Deadlock | Mutex locked twice, missing unlock |

---

## VS Code / IDE Integration

Stasha has editor support via the `stasha tokens`, `stasha symbols`, and `stasha definition` subcommands. These power language server integrations for:

- Syntax highlighting (token stream)
- Go-to-definition
- Symbol lookup

Check your IDE extension for Stasha support.
