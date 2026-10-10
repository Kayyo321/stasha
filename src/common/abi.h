#ifndef STASHA_ABI_H
#define STASHA_ABI_H
/* ABI 2 corrects C, union and tagged-payload layout and scalar semantics.
 * Source-backed archives produced by older compilers must be rebuilt. */
#define STASHA_ABI_VERSION 2
#define STASHA_ABI_MAGIC "STASHA_ABI"
#endif
