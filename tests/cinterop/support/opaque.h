#ifndef CINTEROP_OPAQUE_H
#define CINTEROP_OPAQUE_H

#include <stdint.h>

/* LLVMModuleRef-style: pointer-to-incomplete-struct typedef. */
typedef struct OpaqueFoo  *FooRef;
typedef struct OpaqueBar  *BarRef;

FooRef  foo_create(int32_t seed);
int32_t foo_get(FooRef f);
void    foo_destroy(FooRef f);

BarRef  bar_from_foo(FooRef f);
int32_t bar_get(BarRef b);
void    bar_destroy(BarRef b);

#endif
