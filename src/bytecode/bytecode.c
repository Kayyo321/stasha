#include "bytecode.h"

#include <string.h>

bytecode_t make_bytecode(void) {
    bytecode_t bc = {
        .stack_string_idx = 0,
        .count = 0,
        .capacity = 0,
        .lines = Null,
        .operations = Null,
        .lines_heap = NullHeap,
        .operations_heap = NullHeap,
    };

    memset(bc.stack_string_buf, 0, StackStringBufMax);

    return bc;
}

void free_bytecode(bytecode_t *bc) {
    if (!bc)
        return;

    deallocate(bc->lines_heap);
    deallocate(bc->operations_heap);

    *bc = make_bytecode(); // zero out
}

static boolean_t is_nullheap(heap_t *h) {
    return (!h) || (memcmp(h, &NullHeap, sizeof(heap_t)) == 0);
}

void scribe(bytecode_t *bc, op_code_t op, usize_t ln) {
    if (!bc)
        return;

    if (bc->count + 1 >= bc->capacity) {
        bc->capacity = (bc->capacity <= 0) ? 4 : bc->capacity * 2;

        if (is_nullheap(&bc->operations_heap) || is_nullheap(&bc->lines_heap)) {
            bc->operations_heap = allocate(bc->capacity, sizeof(op_code_t));
            bc->lines_heap = allocate(bc->capacity, sizeof(usize_t));
        } else {
            bc->operations_heap = reallocate(bc->operations_heap, sizeof(op_code_t) * bc->capacity);
            bc->lines_heap = reallocate(bc->lines_heap, bc->capacity * sizeof(usize_t));
        }

        bc->operations = bc->operations_heap.pointer;
        bc->lines = bc->lines_heap.pointer;
    }

    bc->lines[bc->count] = ln;
    bc->operations[bc->count++] = op;
}

void push_stack_str(bytecode_t *bc, char *str, usize_t len)
{
    if (!bc || bc->stack_string_idx + len >= StackStringBufMax)
        return;

    memmove(bc->stack_string_buf + bc->stack_string_idx, str, len);
    bc->stack_string_idx += len;
    bc->stack_string_buf[bc->stack_string_idx++] = '\0';
}
