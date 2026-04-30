#include "opaque.h"
#include <stdlib.h>

struct OpaqueFoo { int32_t v; };
struct OpaqueBar { int32_t v; };

FooRef foo_create(int32_t seed) {
    FooRef f = (FooRef)malloc(sizeof(struct OpaqueFoo));
    f->v = seed * 2 + 1;
    return f;
}

int32_t foo_get(FooRef f) { return f->v; }
void    foo_destroy(FooRef f) { free(f); }

BarRef bar_from_foo(FooRef f) {
    BarRef b = (BarRef)malloc(sizeof(struct OpaqueBar));
    b->v = f->v + 1000;
    return b;
}

int32_t bar_get(BarRef b) { return b->v; }
void    bar_destroy(BarRef b) { free(b); }
