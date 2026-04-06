#ifndef EXAMPLES_CHEADER_MYLIB_H
#define EXAMPLES_CHEADER_MYLIB_H

typedef struct {
    int x;
    int y;
} mylib_point_t;

int mylib_sum(const mylib_point_t *p);

#endif
