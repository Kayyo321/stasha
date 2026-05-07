#ifndef CINTEROP_GLOBALS_H
#define CINTEROP_GLOBALS_H

#include <stdint.h>

/* C globals defined in globals.c, declared here for the cheader scanner.
 * Storage-class qualifiers (extern, static) on declarations are accepted
 * and dropped — only the bare name + type matter for FFI. */
extern int32_t glob_counter;
extern int32_t glob_array[4];

void glob_increment(void);
int32_t glob_read(void);
void glob_set(int32_t v);

#endif
