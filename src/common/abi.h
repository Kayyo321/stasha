#ifndef STASHA_ABI_H
#define STASHA_ABI_H
/* ABI 11 requires rebuilding source-backed archives after ordinary local
 * descriptor-leaf empty defaults and dynamic null object/table/slot guards.
 * Earlier retained bodies bypass these semantics. Physical layout is unchanged;
 * explicit transfer validity, table authenticity, object pairing, lifetime and
 * concurrency are not certified by this semantic marker.
 * ABI 10 requires rebuilding source-backed archives after bounded immediate
 * nominal mismatch rejection at ordinary named-direct arguments and explicit
 * single-result returns to ordinary concrete interface implementers. Native
 * layout is unchanged. Matching annotations, prior stores, helper histories,
 * unsupported routes and projection eligibility are not newly certified.
 * ABI 9 requires rebuilding source-backed archives after checked exact ordinary
 * concrete local transfers and bounded copy-then-use field/fixed-array proofs.
 * Earlier retained bodies bypass these local checks. Descriptor bytes and native
 * ABI layout are unchanged. Helper/result closure, descriptor validity, general
 * ownership/lifetime and runtime CFI remain outside this semantic marker.
 * ABI 8 requires rebuilding source-backed archives after bounded interface
 * source-call argument checks. Earlier retained bodies bypass those checks.
 * Descriptor bytes and slot order are unchanged. Prior stores, ordinary helper
 * transfers, return bodies, initialization, lifetime and CFI are not certified.
 * ABI 7 requires rebuilding source-backed archives after canonical ordinary
 * interface declaration/binding checks and native table/call ABI guards. It is
 * a semantic boundary; descriptor layout is unchanged. Source-flow, generic,
 * initialization, ownership/lifetime and runtime CFI remain separate work.
 * ABI 6 requires rebuilding source-backed archives after interface graph
 * validation and unique-origin diamond slot ordering. Descriptor bytes are
 * unchanged; this marker does not certify interface signature or memory safety.
 * ABI 5 requires rebuilding source-backed archives after checked explicit-count
 * make and source-level slice element conversion changes. Slice descriptor and
 * native C physical layouts are unchanged by this semantic archive marker.
 * ABI 4 requires rebuilding source-backed archives after checked access and
 * logical-length view semantics changes; the descriptor layout is unchanged.
 * ABI 3 adds classified C aggregate calls and explicit static/instance receivers.
 * ABI 2 corrected C, union and tagged-payload layout and scalar semantics.
 * Source-backed archives produced by older compilers must be rebuilt. */
#define STASHA_ABI_VERSION 11
#define STASHA_ABI_MAGIC "STASHA_ABI"
#endif
