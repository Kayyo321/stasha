#ifndef BytecodeH
#define BytecodeH

#include "op.h"
#include "../common/common.h"

enum {
    StackStringBufMax = 1028,
};

typedef struct {
    char stack_string_buf[StackStringBufMax]; // 'xyz' 0 'abc' 0 'another string' 0 0000000
    usize_t stack_string_idx;

    op_code_t *operations;
    usize_t *lines;

    heap_t operations_heap;
    heap_t lines_heap;

    usize_t count, capacity;
} bytecode_t;

bytecode_t make_bytecode(void);
void free_bytecode(bytecode_t *bc);

void scribe(bytecode_t *bc, op_code_t op, usize_t ln);
void push_stack_str(bytecode_t *bc, char *str, usize_t len);

#endif