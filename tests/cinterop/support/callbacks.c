#include "callbacks.h"

static cb_int_op g_cb = 0;

void    cb_register(cb_int_op cb) { g_cb = cb; }
int32_t cb_invoke(int32_t n)      { return g_cb ? g_cb(n) : -1; }
int32_t cb_doubler(int32_t n)     { return n * 2; }

int32_t cb_find(const int32_t *arr, int32_t n, int32_t (*pred)(int32_t)) {
    for (int32_t i = 0; i < n; i++)
        if (pred(arr[i])) return i;
    return -1;
}
