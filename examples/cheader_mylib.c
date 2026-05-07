#include "cheader_mylib.h"

int mylib_sum(const mylib_point_t *p) {
    return p ? p->x + p->y : 0;
}
