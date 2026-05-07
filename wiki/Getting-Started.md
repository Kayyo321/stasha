# Getting Started with Stasha

This guide walks you from zero to a running Stasha program in a few minutes.

---

## Prerequisites

- macOS (ARM64 or x86_64) or Linux (x86_64)
- A C/C++ toolchain (`clang`, `make`)
- LLVM (built automatically, or provided)
- Git

---

## Building the Compiler

Stasha is self-contained. Clone the repository and run `make`:

```bash
git clone https://github.com/YOUR_USERNAME/stasha.git
cd stasha
make
```

This produces the `bin/stasha` compiler binary.

To also build the standard library:

```bash
make stdlib
```

---

## Verifying the Installation

```bash
bin/stasha --version
```

For convenience, add `bin/` to your PATH:

```bash
export PATH="$PATH:/path/to/stasha/bin"
```

---

## Your First Program

Create a file called `hello.sts`:

```stasha
mod hello;

ext fn main(void): i32 {
    print.('Hello, World!\n');
    ret 0;
}
```

Compile and run it:

```bash
stasha hello.sts
./hello
Hello, World!
```

Or specify an output name:

```bash
stasha hello.sts -o greet
./greet
Hello, World!
```

---

## Understanding the Program

Let's break it down line by line:

```stasha
mod hello;
```

Every Stasha file starts with a **module declaration**. The name `hello` matches the filename. This tells the compiler this is the `hello` module.

```stasha
ext fn main(void): i32 {
```

- `ext` — this function is **exported** (externally visible, like `public`)
- `fn` — function declaration keyword
- `main` — the entry point
- `(void)` — takes no parameters
- `: i32` — returns a 32-bit signed integer (exit code)

```stasha
    print.('Hello, World!\n');
```

`print.()` is a built-in formatted output function. No import required. The `.` after `print` is part of Stasha's built-in call syntax. Single or double quotes both work for string literals.

```stasha
    ret 0;
```

`ret` is the return keyword. `0` means success.

---

## A More Interesting Program

```stasha
mod greet;

ext fn main(void): i32 {
    i32 count = 3;

    for i32 i = 0; i < count; i++ {
        print.('Greetings, number {}!\n', i + 1);
    }

    ret 0;
}
```

Output:
```
Greetings, number 1!
Greetings, number 2!
Greetings, number 3!
```

New things here:
- `i32 count = 3;` — a stack-allocated 32-bit integer variable
- `for init; cond; update {}` — familiar C-style for loop
- `{}` inside `print.()` — inserts the next argument

---

## Project Mode

For larger programs, use a project file. Create `sts.sproj` in your project directory:

```
main   = "src/main.sts"
binary = "bin/myapp"
```

Then just run:

```bash
stasha
```

Stasha reads `sts.sproj` automatically and builds your project.

---

## Next Steps

- [Language Basics](Language-Basics) — Learn the core syntax
- [Variables](Variables) — Understand storage qualifiers
- [Functions](Functions) — Write and call functions
- [Memory Management](Memory-Management) — Understand how memory works in Stasha
