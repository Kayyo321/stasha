#ifndef CINTEROP_STRUCTS_H
#define CINTEROP_STRUCTS_H

#include <stdint.h>

/* Anonymous-struct-typedef pattern (row 8). */
typedef struct {
    int32_t x;
    int32_t y;
} point_t;

typedef struct {
    point_t origin;
    point_t size;
} rect_t;

typedef struct {
    int32_t *data;
    int32_t  n;
} islice_t;

typedef struct {
    int32_t buf[8];
    int32_t n;
} ibuf_t;

point_t point_make(int32_t x, int32_t y);     /* return by value */
int32_t point_dot(point_t a, point_t b);       /* args by value */
int32_t point_dot_ptr(const point_t *a, const point_t *b);
int32_t rect_area(const rect_t *r);
int32_t slice_first(islice_t s);
int32_t ibuf_sum(const ibuf_t *b);

#endif
