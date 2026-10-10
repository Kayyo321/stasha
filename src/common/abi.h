#ifndef STASHA_ABI_H
#define STASHA_ABI_H
/* ABI 3 adds classified C aggregate calls and explicit static/instance receivers.
 * ABI 2 corrected C, union and tagged-payload layout and scalar semantics.
 * Source-backed archives produced by older compilers must be rebuilt. */
#define STASHA_ABI_VERSION 3
#define STASHA_ABI_MAGIC "STASHA_ABI"
#endif
