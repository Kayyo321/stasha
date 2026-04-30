#include "structs.h"

point_t point_make(int32_t x, int32_t y) {
    point_t p = { x, y };
    return p;
}

int32_t point_dot(point_t a, point_t b) {
    return a.x * b.x + a.y * b.y;
}

int32_t point_dot_ptr(const point_t *a, const point_t *b) {
    return a->x * b->x + a->y * b->y;
}

int32_t rect_area(const rect_t *r) {
    return r->size.x * r->size.y;
}

int32_t slice_first(islice_t s) {
    return s.n > 0 ? s.data[0] : -1;
}

int32_t ibuf_sum(const ibuf_t *b) {
    int32_t sum = 0;
    for (int32_t i = 0; i < b->n; i++) sum += b->buf[i];
    return sum;
}
