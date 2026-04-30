#include "globals.h"

int32_t glob_counter = 100;
int32_t glob_array[4] = { 1, 2, 3, 4 };

void glob_increment(void) { glob_counter++; }
int32_t glob_read(void)   { return glob_counter; }
void glob_set(int32_t v)  { glob_counter = v; }
