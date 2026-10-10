#ifndef STASHA_ABI_H
#define STASHA_ABI_H
/* ABI 7 requires rebuilding source-backed archives after canonical ordinary
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
#define STASHA_ABI_VERSION 7
#define STASHA_ABI_MAGIC "STASHA_ABI"
#endif
