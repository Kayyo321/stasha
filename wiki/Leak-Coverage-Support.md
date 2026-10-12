# Leak and Coverage Checker Support

**Status: partial, opt-in, developmental.** The goal is support for all
implemented Stasha language features. The checker stays opt-in until complete
support is independently qualified for the release's declared execution targets.
Coverage thresholds are **always explicit user choices**: completing feature
support does not introduce a default minimum or a requirement for 100% coverage.

See the [observation guide](../tests/observation/README.md) for the current flags,
report contract and qualification procedure.

## What complete support means

Feature compatibility and measurement are separate obligations. Every
otherwise-valid implemented feature must work in an observed test image without
an observation-only rejection, preserve ordinary program behavior, and have a
defined source, allocation and completion contract. Accepting syntax while
silently missing its generated allocations or executable source is not support.

A clean result describes **one completed execution**, its configured input and
provider image, its declared allocation domain, and its endpoint. It is not a
static proof of leak freedom, ownership or pointer safety. Unexecuted paths,
different inputs and thread schedules remain unmeasured. Even 100% executable
statement/control-line coverage is not branch or path coverage.

An opaque external effect may eventually be accepted with an explicit
incomplete/unmeasured result. That is useful compatibility progress, but cannot
close a fully measured support row or justify blanket default activation.

## Current admission profile

The v4 design is limited to an explicit on-disk root `stasha test`
invocation with optional distinct direct ordinary leaf-source imports on hosted native **Linux x86-64 GNU/glibc LP64**. It admits sequential,
directly named non-generic functions; primitive scalar/pointer values and fixed
primitive nonpointer-element arrays; bounded local explicit-count and positional
builtin primitive slice backing with closed retained/direct-rem/adjacent-defer schedules; bounded
ordinary control flow and defer;
explicit `new`, two-argument `mov`, and `rem`; primitive assertions and printing;
and selected compile-time/target conditionals. The detailed restrictions are in
the [observation guide](../tests/observation/README.md#admitted-source-profile).

Projects, transitive imports, explicit aliases, providers, other artifact modes, stdin overrides, additional
libraries, and other targets are outside this profile. The source inventory
rejects unsupported constructs even in uncalled functions. The exact profile is `native-sequential-direct-source-positional-slice-v4`, with observer ABI and
raw-report/driver-JSON/manifest schema 4, allocation metric 2 and line metric 2.
Input graph bytes/schema and family-evidence receipt schema remain v2/2. Root imports are
unconditional plain imports; leaf imports, duplicate modules/canonical files and
ambiguous aliases are rejected. Imported tests receive their own lexical module
context and identity. Requested entry-directory dependency lookup is preserved.
Admission is not a
qualification result: compiler builds, source review, IR review and component
tests do not establish successful execution of a generated test image.

### Allocation and coverage boundaries

- `tracked_outstanding_bytes` sums requested sizes of live successful explicit
  `new`/`mov` allocations plus admitted generated primitive-slice backing, at the
  end of the whole test suite. Separate explicit-user-heap and primitive-slice
  backing totals sum to the combined thresholded total. Production and test
  allocations both count; a later test may release an earlier test's buffer.
  There is no per-test subtraction. Reachable retained buffers still count.
- Allocation count is separate from bytes. A nonnull zero-byte allocation can
  warn while meeting a zero-byte maximum. Failed nonzero resize preserves the
  original record. Reached zero-size resize and unknown/non-base free or resize
  fail incomplete before invoking the allocator. Address generations do not
  establish pointer freshness or ownership.
- Stack/static storage, allocator slack, observer/compiler bookkeeping and
  libc/runtime-private memory are outside the current metric. File descriptors,
  sockets, OS handles, locks and device resources are not heap-byte measurements.
  Missing language-generated or provider allocation support is a gap, not an
  implied zero allocation count.
- Coverage is **executable statement/control-line coverage**, deduplicated by
  `(file_id, source line)`. Global coverage uses the union of those pairs; file
  percentages are never averaged. Independently evaluated conditions, short-circuit operands and
  conditional arms need their own anchors; defers hit when their bodies execute.
  Uncalled production functions remain in the pre-optimization denominator.
- Test bodies and the replaced application `main` are excluded from production
  coverage, while test allocations remain tracked. Compile-time-only, inactive
  and structurally non-emitted source has explicit exclusion reasons. Unknown or
  unsupported source cannot silently disappear from the denominator.
- Inclusive thresholds use exact arithmetic. A requested coverage minimum fails
  with zero eligible lines, even when that minimum is zero. Zero tests fails;
  zero assertions in a normally returned test is allowed and reported. Assertion
  failure, abnormal termination or an absent/invalid/incomplete report fails the
  command. Warning-only outstanding allocations do not themselves fail it.

## Full-language work inventory

The [AST inventory](../src/ast/ast.h) currently contains 107 node kinds. This is an
inventory count, **not a count of qualified features**. Types, declaration flags,
headers, lowering variants, provider modes and targets add further dimensions.
The groups below summarize that inventory without treating parser acceptance as
proof of correct lowering or observation.

| Feature family | Remaining support and qualification work |
|---|---|
| Scalars, pointers, arrays, variables and ordinary control | Qualify the admitted subset; extend storage/type combinations, atomic/volatile/TLS/restrict, aliases/inference, comparison chains, `with`, foreach and multi-return/destructuring. Preserve evaluation order, diagnostics and cleanup on every exit. |
| Functions and source names | Expression bodies, recursion, quoted/long names, linkage and exports need stable resolved identities and collision-safe hooks; application `main` needs an explicit execution policy. |
| Structs, unions, enums, inline tagged values and interfaces | Cover layout/attributes/bitfields, members, methods, constructors, pattern bindings, dispatch, nested fields and destructors. Interface/provider integration remains an open qualification dependency. |
| Fixed primitive-array len/cap queries | Preserve the published bounded identifier/remaining-rank proof, ten ordinary sources, five generator dependencies and exact 8/10 oracle under observer v4. Third-family source/IR, native and final aggregate gates remain mandatory. Borrowed-view/member/alias/parameter/unsafe queries remain open. |
| Local explicit-count primitive slice backing | Fourth-family bounded increment: exact local builtin element identity, direct-live scalar queries/indexes and retained/direct/adjacent-defer schedules. Independent source/IR, pure controls, 188 ordinary and 800 observed native runs and final five-family aggregate are required. Native malloc-null remains NOT_RUN. |
| Local positional primitive slice backing | Fifth-family bounded direct `heap []T s = make.{items}` with recursive typed scalar items and the same three schedules. Exact46 positives require 184 ordinary then784 observed O0–O3 runs;102 rejections and 4 item-limit controls stay nonlaunch. Full source/IR, frozen metadata/KAT, pure and final five-family gates remain required; native OOM stays NOT_RUN. |
| General initializers, slices and views | Compound/designated initialization, spread/range/merge, general `make`, append/growth, copy, aliases, transfer/return/capture, composite elements and other queries stay open. Descriptor copies and interior views are not new allocations or ownership proof. |
| Errors, formatting and generated helpers | Classify heap versus stack/static buffers, propagation cleanup, hash/equality, type queries and rich assertions. Generated allocations need source attribution and matching release events. |
| Generics, lambdas and variadics | Preserve template/specialization/instantiation origins; lifted and captured bodies; function pointers, callbacks, pipelines/trailing closures; raw/typed variadic ABI and state. Track actual environment storage, not an assumed heap allocation for every capture. |
| Modules, macros and compile-time forms | The bounded v2 profile adds immutable direct-source graphs, per-file coverage, owned test identities and rejection of repeated imports. General transitive imports, deduplication policy, submodule/project tests and macro definition/expansion origins remain open. Freeze separate source-line and specialization/expansion reporting rules. |
| Zones and generated cleanup | Account for zone state, backing blocks, logical requests, bulk release and independent move-out allocations. Cover normal return, break/continue, error propagation, defer and nested destructor paths. |
| Lifecycle and signals | Include startup/initialization and teardown, watcher tables/captures/handlers, reentrant dispatch and retained registrations. An early quit or crash cannot manufacture completed-suite status. |
| Tasks, streams and threads | Account for coroutine frames, suspension/resumption, await/combinators, cancellation/drop and result ownership; thread job arguments, futures/results and TLS teardown. Require safe concurrent ledger/hit updates and a quiescent endpoint. |
| Headers, unsafe, assembly and foreign effects | Preserve valid layout/linkage/target attributes and legal unsafe/unchecked behavior. Hidden allocator, syscall or control effects require explicit contracts; opaque effects cannot receive a complete measurement result. |

The canonical compile-time assertion spelling is `@comptime assert.(...)`.
Qualification fixtures use it. The legacy function-body spelling
`comptime_assert.(...)` has an existing lowering defect and remains unqualified;
its implementation is not changed by this roadmap. Existing language defects
remain explicit issues rather than observation successes.

### Allocation domains, providers and targets

Full support must inventory allocations emitted by the compiler **into the
target program**, plus linked language runtimes and qualified providers. It must
cover slice growth, formatted/error buffers, generated field cleanup, coroutine
frames, thread/future storage, zone backing and signal tables, not just explicit
user `new` calls. Compiler-process heap tracking supplies no target leak evidence.

An expanded metric needs a versioned domain contract. Keep user heap,
language-runtime backing and qualified-provider totals distinguishable. Zone
logical request bytes and physical backing bytes are two non-additive views;
select their threshold meaning before implementation. Ownership transfer changes
attribution, not allocation count. Unknown size or allocator domain stays unknown,
never zero. Ledger overflow, lost provenance or exhausted capacity is incomplete.

C/standard-library/custom allocators need reviewed adapters or instrumented
providers with source, allocator/deallocator, size and lifetime contracts. An ABI
marker, header, returned handle or source-backed archive alone does not establish
measurement of foreign internals. Library-only builds need a qualified consumer
image; they cannot report an execution that did not occur.

Every supported host/ABI needs its own runtime, packaging and execution evidence:
other Linux architectures/libcs, macOS, Windows and other hosted targets are open
rows. Cross-compiled objects need a compatible runner before runtime results can
be claimed. Freestanding/kernel targets require target-specific allocation/event
sinks and startup, shutdown and completion protocols; hosted observer machinery
and object/IR checks are not kernel execution evidence.

## Phased implementation and positive qualification

Each phase closes complete feature slices, including source mapping, allocation
events and execution evidence. Broadening admission first and filling in missing
measurement later cannot produce a complete result. The checker remains opt-in
through intermediate phases.

1. **Freeze the contract.** Version the release feature/target inventory,
   allocation domains, source denominator, provider completeness, endpoint and
   report schema. Classify new AST/type/flag additions before admitting them.
2. **Qualify the narrow profile.** Independently check balanced and deliberate
   outstanding allocations, exact line sets, thresholds at and around equality,
   test-only work, suite-wide cleanup, zero denominators, assertions and status.
   Review lowering and qualify conventional valid programs at O0–O3.
3. **Generalize source identity.** Qualify direct-source v2, then extend to projects, owned macro/generic
   origins, lifted bodies and stable IDs. Test same-line/file collisions, repeated
   imports/instantiations, uncalled bodies and optimization-invariant denominators.
4. **Centralize generated allocation and cleanup.** Add reviewed emission/runtime
   adapters and ordinary aggregate, slice, error/format and helper families.
   Demonstrate balanced and deliberately retained backing plus every normal
   cleanup edge; inspect the linked allocator inventory for bypasses.
5. **Complete ordinary language combinations.** Qualify layout, methods,
   generics/interfaces, callbacks/closures, variadics, richer control and legal
   unsafe/assembly forms. Compare ordinary and instrumented outputs and side
   effects, including high-risk combinations such as generic destructors.
6. **Close lifetime and concurrency domains.** Add zones and startup/teardown;
   then signals, coroutines and real threaded execution. Qualify early drop,
   cancellation, joins, TLS cleanup and exact final accounting after quiescence.
7. **Qualify providers and host ports.** Start with small deterministic providers,
   then standard-library families and each declared host/ABI. Verify clean
   installed packages, runner provenance and provider boundaries independently.
8. **Audit complete support before default activation.** Close every applicable
   feature/provider/target row and interaction gap, then evaluate the default UX,
   resource costs and installation/editor/harness behavior as a release decision.

For each row, establish expected semantics, allocation counts/bytes and literal
source hit/denominator sets independently of the compiler under test. Use pure
ledger IDs, callback spies and malformed-report fixtures for bookkeeping/failure
controls; these do not substitute for end-to-end execution. Native positive
fixtures use ordinary defined behavior, including intentional retained
allocations, without invalid allocator operations.

Keep exact source/compiler/runtime/provider/configuration/image identities,
commands, pre/post-lowering review, raw reports, independent recomputation and
actual child/test outcomes. Distinguish source-only, IR-only, component, linked,
executed and aggregate evidence. Existing ordinary regression suites are useful
inputs, not automatic checker qualification. Measure performance and bounded
resource behavior after correctness; exceeded limits must not truncate to success.

## Default-activation gate

Default activation requires all of the following:

- Every implemented release feature and supported target has complete, matching
  independent evidence; no observation-only rejection or unknown in-domain effect
  remains behind a successful complete result.
- All user/generated/runtime/provider allocation routes in the declared domain
  and their cleanup paths are accounted for. Source provenance and the full
  denominator survive imports, expansion, specialization, cloning and optimization.
- Startup, callbacks, workers, coroutines and teardown reach a genuine endpoint;
  assertion failure, abnormal/incomplete execution and report failure cannot pass.
- Provider boundaries, host/runner contracts and resource limits are explicit;
  unresolved integrations remain open rather than being relabeled as supported.
- The exact final artifacts have conventional end-to-end and clean-install
  evidence, including editor/harness behavior. Earlier or different-tree results
  cannot qualify them.
- The default command policy explicitly handles no tests, test versus application
  execution, output transactions and actionable failures. Caller migration waits
  for that decision and full qualification.

**Coverage minimums remain explicit-only after activation.** Full language
compatibility never implies complete path execution or a static leak-freedom proof.

### Fixed-array query evidence boundary

The inherited bounded query increment is requalified under observer ABI/report
schema 4 and allocation/line metrics 2/2 without changing its ordinary inputs
or literal coverage/semantic oracles. The permanent family retains ten exact ordinary sources and their
single-main-rename adapters, six independently frozen fixtures, strict runtime
identity checks, and no-launch-only negative controls. Its central 8/10 `ff00`
oracle has five assertions and two successful balanced 8-byte helper allocations;
auxiliary cases cover index/control anchors, retained 13-byte helper storage,
later-test cleanup and distinct root/import source owners. Source/IR or pure-only
results are preparation evidence, not successful native qualification. Close only
this bounded row after all required gates on the same final candidate; all other
query/storage/provider/target rows and default activation remain open.

### Primitive-slice evidence boundary

The measured generated domain includes bounded local explicit-count and positional
builtin primitive slice backing in the declared sequential hosted Linux x86-64 LP64 profile.
Positive capacity requests exactly capacity times target ABI element stride;
zero capacity makes no allocator event. Source/IR must establish full-width count
validation, once-only count order, reason-8 null dominance, full-capacity zeroing,
effectful real allocator hooks and every ordinary cleanup exit. Native assertions
never read spare capacity. The separately reviewed 47-case inventory includes
all eleven primitive types, integer count widths, zero/retained/mixed domains,
source imports, same-line sites, nested/shadowed bindings and three schedules.

Pure synthetic-ID/allocator-spy and complete parser/serialization controls prove
bookkeeping/failure contracts without native invalid allocator operations.
The original 177 rejection source texts remain preserved; exactly three
inline-make cases move to the fifth-family positive inventory. The other 174
rejections and all 22 count-failure sources remain nonlaunch-only.
Native positive reports must independently match source, image, profile/config/map,
site/domain/generation and per-domain requested-byte totals. All five mandatory
families must reach exact terminal receipts on unchanged final artifacts.

Genuine native malloc-null remains **NOT_RUN / outside initial native qualification**.
An inspected reason-8 branch or a callback that returns NULL does not establish
native OOM. Unsupported slices/aliases/transfers/growth, providers, generated
allocation families, broad ownership/lifetime analysis and other platforms remain
open. These gates neither close full-language support nor enable default checking,
implicit coverage minimums, restricted descriptor/archive work or LeakSanitizer.


### Positional-slice evidence boundary

Observer/schema/profile4 adds bounded direct primitive positional make to existing
domain 2, while allocation/line metrics 2/2 and source graph 2 remain unchanged.
Kind 6 carries exact positional N; live bytes must equal N×stride. Empty/dead sites
remain in the static inventory without inventing allocator events. Per-constructor
and aggregate selected-source item limits are65,536. Ordered original item and
binding identities, source semantic types and compatible ordinary conversions
are checked even in uncalled/dead source. Initialized reads are a defined-behavior
fixture precondition; no new definite-initialization proof is claimed.

All 184 independent ordinary positives must pass before any of 784 observed
invocations. Ordinary as-if allocation elision remains permitted. Source/IR and
pure controls establish null-before-access, order and cleanup routes; native
reports establish suite completion, executed hits and live endpoint attribution,
not an allocation-event trace or safety for unexecuted paths. Exactly three old
inline-make rejections are promoted with source bytes unchanged.

Transitive source imports still need a substantive ordinary resolver repair:
current ordinary import processing discards leaf import nodes before splicing,
so comments cannot establish transitive behavior. General initializers,
range/spread/designators, direct aggregate/index/query item forms, aliases,
views/transfer/capture/return/growth, composite elements, generated providers,
other targets and broader lifetime/ownership analyses remain open. The eventual
100% language-support goal remains; until qualified, the incomplete checker is
opt-in and coverage thresholds explicit-only.
