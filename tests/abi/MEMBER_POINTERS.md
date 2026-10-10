# Member storage and receiver regression

Run `python3 tests/abi/run_member_pointers.py --compiler bin/stasha --artifacts /tmp/member-results`.

Each O0–O3 run compiles a fresh source object, links it with an independent C
layout/value oracle, executes with a timeout, and retains stdout, stderr, process
categories and compiler identity. C initializes and inspects the actual pointee
storage. Coverage includes direct pointers, embedded structs, pointer-valued
fields, fixed arrays, compound writes and address-of; a deliberately unaligned
packed object has checked canary bytes on both sides.

The same executable checks ordinary, generic and imported static factories,
zero-argument factories, calls through both type and object syntax, instance
methods and sibling dispatch, and a C ABI static method called directly and
through a C function pointer. Negative fixtures require exact diagnostics for
read-only writes, missing/extra arguments, receiverless instance calls (including
generic and module-qualified calls), and use of `this` in external static methods.

Receiver behavior follows the declaration location: methods inside a type have
an implicit receiver; `fn Type.method` declarations outside it are static and have
exactly their declared parameters. No null receiver is synthesized. External
static method ABI changed and all Stasha archives must be rebuilt at the current
ABI version. Destructors belong inside the type definition.

These tests qualify the covered native C/object boundary only. They do not prove
complete pointer provenance, permissions, lifetime safety, or other targets.
