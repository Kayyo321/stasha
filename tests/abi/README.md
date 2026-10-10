# C call ABI qualification

Run on GNU/Linux x86-64 LP64 with the repository's pinned Clang/LLVM toolchain:

```sh
python3 tests/abi/run_sysv_aggregates.py bin/stasha --cc clang
```

The script uses a fresh artifact directory by default. `--out DIR` retains
sources, IR, objects, executables, diagnostic logs and a machine-readable report.
Do not reuse a directory containing old outputs when validating failure cleanup.

At O0 and O2, independently:

- 79 scalar/pointer-only object-layout probes (including 72 natural/packed
  integer/floating pairs) run before the first aggregate call
- Each aggregate is called from Stasha to C through an inferred function pointer,
  from C to Stasha directly, and from C through a Stasha-supplied callback
- Small and large nested records, SSE pairs/triples, odd-width fixed arrays,
  integer/SSE unions, MEMORY arguments and sret results are covered
- Seven register-pressure tests cover both directions, rollback when an entire
  aggregate cannot fit, mixed register banks and the hidden sret register
- Narrow signed/unsigned C integer calls and their LLVM extension attributes
  are checked
- Seven explicit rejection cases cover foreign targets, pointer ABI erasure,
  aggregate varargs, inferred C aggregate signatures, incompatible pointer
  signatures, source array parameters and bitfields

The C-oracle transformations use ordinary indexed assignment so object/call ABI
checks remain separate from lvalue lowering. The scalar suite independently
checks compound assignment through fixed-array fields and multidimensional
indices at O0–O3.

`run_c_callbacks.py` separately tests imported typed callback typedefs, inline
callback parameters, callback factories, small/large aggregate callbacks and
native-versus-C convention rejection at O0/O2.
`run_c_callback_parameters.py` checks one-dimensional fixed/unsized array
parameter decay, typedef/const preservation and direct calls at O0–O3, with
nine explicit unsupported declarator-shape rejections.

`docs/C_ABI.md` describes the implemented profile and excluded cases. These
results qualify only the documented C declarator subset and target. They do not
qualify other platforms, arbitrary C syntax, C++ ABI, ownership or general memory safety.
