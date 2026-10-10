# Source-constant integer conversions

Run the dedicated regression complement with the provenance-verified compiler:

```sh
python3 tests/scalars/test_constant_conversions.py --compiler ./bin/stasha \
  --opt 0,1,2,3 --artifacts /tmp/stasha-constant-conversions
```

The script reuses the P1 fail-closed runner, retains source/compiler/output hashes,
checks exact runtime output, and requires normal diagnostic exit 1 with a source
location for every rejected program. `--list` inspects its deterministic inventory
without qualifying any compiler.

## Conversion contract

The normal type-wide implicit conversion rules are unchanged. In addition, a
supported source-level constant expression may narrow or change signedness when
its exact mathematical value fits the target range. This includes `sizeof.(T)`,
integer literals and casts, supported integer arithmetic, and previously
initialized module-private `int const` integers. For example, `u32 n = sizeof.(u64)`
preserves the value 8. The proof evaluates the expression in its original source
type, then checks the destination bounds before any truncation or sign change.

Signed sources are interpreted as signed. `int const i64 n = -1` cannot initialize a
`u64`, and `int const u64 n = 9223372036854775808` cannot initialize an `i64`.
An explicit cast still performs the existing intentional bit conversion.
Arithmetic inside a constant expression retains its source-width wrapping and
trapping semantics; this is not unbounded arithmetic or a new arithmetic policy.

The proof uses the source constant engine, not the LLVM optimizer, a constant
LLVM instruction, or a folded local load. Runtime locals (including local
`const`/`final` declarations), parameters, and calls require an explicit cast when
the type-wide conversion may lose value. Mixed-sign arithmetic still requires an
explicit cast even when both operands are module constants. Existing P1
runtime-variable narrowing and signedness rejection cases remain valid.

Identifier-based runtime proofs require internal/private linkage. Weak or
replaceable definitions and externally initialized globals do not establish an
exact value. Strong externally visible definitions (including default `const`
globals) also cannot establish a runtime narrowing proof: shared-library symbol
preemption can replace their value. This restriction is independent of LLVM
optimization. Ordinary global/comptime evaluation may still use a strong const
initializer because that evaluation emits a fixed constant rather than a runtime
load. Existing type-wide safe conversions remain available for exported globals.

Implicit pointer, array-decay, function-address, and string-to-integer conversions
are rejected, including integer-typed arguments, returns, and assignments. This
also closes expression forms whose source type is unresolved: their pointer
shape cannot fall through to the legacy implicit `ptrtoint` helper. Explicit
pointer/integer casts remain available for low-level code. Pointer truth tests
in conditions retain their existing behavior.

## Coverage

- Every signed/unsigned 8/16/32/64-bit source/destination pair at fitting boundary
  values in globals, locals, typed constant casts, assignments, arguments, returns
- `sizeof` for builtins, aliases, structs, arithmetic, aggregate elements/fields
- Exact signed/unsigned destination endpoints and just-outside-range failures
- Negative signed values and unsigned high-bit values interpreted from their
  source type, including 64-bit extremes
- Constant arithmetic/selection/shift results and existing wrapping semantics
- Identical acceptance at O0–O3; optimizer-foldable locals/calls remain rejected
- Pointer/string/function/address/array rejection plus explicit cast roundtrips

These checks complement P1. They do not qualify cross-target pointer widths,
floating-point constant folding, or constant evaluation of arbitrary functions.

## Existing fixture correction

P1's `short_circuit` test labeled `'B'` as a character, but Stasha parses single
quotes as a string and backticks as a character. Its literal now uses backticks;
the expected character-to-boolean result and all existing runtime oracles are
unchanged. The old source relied accidentally on the rejected implicit string
pointer-to-integer path. The dedicated conversion suite rejects string-to-bool
explicitly so that this hole cannot be restored just to satisfy the old fixture.
