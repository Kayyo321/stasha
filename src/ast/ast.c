#include <string.h>
#include "ast.h"

/* ── arena: tracks every AST allocation for bulk deallocation ── */

enum { ArenaInitCap = 64 };

static heap_t *arena_entries = 0;
static usize_t arena_count   = 0;
static usize_t arena_cap     = 0;
static heap_t  arena_heap    = {0};

static void arena_track(heap_t h) {
    if (arena_count >= arena_cap) {
        usize_t new_cap = arena_cap < ArenaInitCap ? ArenaInitCap : arena_cap * 2;
        if (arena_heap.pointer == Null)
            arena_heap = allocate(new_cap, sizeof(heap_t));
        else
            arena_heap = reallocate(arena_heap, new_cap * sizeof(heap_t));
        arena_entries = arena_heap.pointer;
        arena_cap = new_cap;
    }
    arena_entries[arena_count++] = h;
}

void ast_free_all(void) {
    for (usize_t i = arena_count; i > 0; i--)
        deallocate(arena_entries[i - 1]);
    arena_count = 0;

    if (arena_heap.pointer != Null) {
        deallocate(arena_heap);
        arena_heap = NullHeap;
        arena_entries = Null;
        arena_cap = 0;
    }
}

/* ── node creation ── */

node_t *make_node(node_kind_t kind, usize_t line) {
    heap_t h = allocate(1, sizeof(node_t));
    arena_track(h);
    node_t *n = h.pointer;
    memset(n, 0, sizeof(node_t));
    n->kind = kind;
    n->line = line;
    return n;
}

char *copy_token_text(token_t tok) {
    heap_t h = allocate(tok.length + 1, sizeof(char));
    arena_track(h);
    char *s = h.pointer;
    memcpy(s, tok.start, tok.length);
    s[tok.length] = '\0';
    return s;
}

char *ast_strdup(const char *src, usize_t len) {
    heap_t h = allocate(len + 1, sizeof(char));
    arena_track(h);
    char *s = h.pointer;
    memcpy(s, src, len);
    s[len] = '\0';
    return s;
}

/* ── node list ── */

void node_list_init(node_list_t *list) {
    list->items = Null;
    list->count = 0;
    list->capacity = 0;
    list->heap = NullHeap;
}

void node_list_push(node_list_t *list, node_t *node) {
    if (list->count >= list->capacity) {
        usize_t new_cap = list->capacity < 8 ? 8 : list->capacity * 2;
        if (list->heap.pointer == Null) {
            list->heap = allocate(new_cap, sizeof(node_t *));
            arena_track(list->heap);
        } else {
            list->heap = reallocate(list->heap, new_cap * sizeof(node_t *));
        }
        list->items = list->heap.pointer;
        list->capacity = new_cap;
    }
    list->items[list->count++] = node;
}
