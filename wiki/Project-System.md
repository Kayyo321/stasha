# Project System

For multi-file programs and libraries, Stasha uses a project file called `sts.sproj`. When you run `stasha` (with no arguments) in a directory that contains `sts.sproj`, it automatically reads the project configuration and builds.

---

## Creating a Project File

Create `sts.sproj` in your project root:

```
main   = "src/main.sts"
binary = "bin/myapp"
```

Then build by running `stasha` with no arguments:

```bash
stasha
./bin/myapp
```

---

## `sts.sproj` Fields

### For Executables

```
main   = "src/main.sts"   # entry point source file
binary = "bin/myapp"      # output executable path
```

### For Libraries

```
main    = "src/lib.sts"
library = "bin/libmylib.a"   # build as static library
```

### External Libraries

```
ext_libs = [
    ("libs/engine.a" : "libs/engine.sts"),
    ("libs/net.a"    : "libs/net.sts"),
]
```

The first path is the archive, the second is the Stasha interface source file (used by the importer for type-checking).

### Full Example

```
main     = "src/main.sts"
binary   = "bin/server"
ext_libs = [
    ("libs/db.a"     : "libs/db.sts"),
    ("libs/crypto.a" : "libs/crypto.sts"),
]
```

---

## Project Directory Layout

A typical Stasha project:

```
myproject/
  sts.sproj
  src/
    main.sts
    math/
      vector.sts
      matrix.sts
    net/
      client.sts
      server.sts
  libs/
    mylib.a
    mylib.sts      (interface for the library)
  bin/
    myapp          (output)
```

---

## Module Names Follow Directory Structure

Module names mirror the path relative to the entry file. If `main.sts` is at `src/main.sts`, then:

| File | Module declaration |
|------|--------------------|
| `src/main.sts` | `mod main;` |
| `src/math/vector.sts` | `mod math.vector;` |
| `src/math/matrix.sts` | `mod math.matrix;` |
| `src/net/client.sts` | `mod net.client;` |

And in `main.sts`:
```stasha
mod main;

imp math.vector;
imp math.matrix;
imp net.client;
```

---

## Build Modes

### Project-Mode Executable

```
stasha              # reads sts.sproj, builds binary
stasha build        # same
```

### Project-Mode Library

If `sts.sproj` has `library = "bin/libname.a"`:

```
stasha              # builds static library
```

---

## The Bootstrap Compiler — A Real `sts.sproj`

Stasha's own bootstrap compiler (being written in Stasha) uses this project file:

```
main   = "src/main.sts"
binary = "../bin/stasha"
ext_libs = []
```

This shows how a real, non-trivial compiler project is organized with `sts.sproj`.

---

## Tips

**Relative paths** — All paths in `sts.sproj` are relative to the file itself (the project root).

**Separate libs from src** — Keep precompiled libraries in `libs/`, source in `src/`, outputs in `bin/`.

**Use `stasha lib` to build dependencies first:**

```bash
stasha lib libs/engine.sts -o libs/libengine.a
stasha   # now build the main project
```

**Check your module declarations match your directory structure.** If `src/net/client.sts` says `mod main;` instead of `mod net.client;`, you'll get confusing import errors.
