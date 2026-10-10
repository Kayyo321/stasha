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

void ast_arena_track(heap_t h) { arena_track(h); }

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

void ast_set_loc(node_t *node, token_t tok) {
    if (!node) return;
    node->line = tok.line;
    node->col = tok.col;
    node->len = tok.length;
    if (tok.file)
        node->source_file = ast_strdup(tok.file, strlen(tok.file));
}

char *copy_token_text(token_t tok) {
    heap_t h = allocate(tok.length + 1, sizeof(char));
    arena_track(h);
    char *s = h.pointer;
    memcpy(s, tok.start, tok.length);
    s[tok.length] = '\0';
    return s;
}

char *copy_name_text(token_t tok) {
    if (tok.kind == TokDollarStr)
        return ast_strdup(tok.start + 2, tok.length - 3); /* skip $" and closing " */
    return copy_token_text(tok);
}

char *ast_strdup(const char *src, usize_t len) {
    heap_t h = allocate(len + 1, sizeof(char));
    arena_track(h);
    char *s = h.pointer;
    memcpy(s, src, len);
    s[len] = '\0';
    return s;
}

ast_source_loc_t ast_source_loc(token_t tok) {
    const char *file = tok.file ? tok.file : diag_get_file();
    return (ast_source_loc_t){file ? ast_strdup(file, strlen(file)) : Null,
                              tok.line, tok.col, tok.length};
}

generic_use_t *ast_generic_use(char *base_name, ast_source_loc_t loc) {
    heap_t h = allocate(1, sizeof(generic_use_t));
    arena_track(h);
    generic_use_t *use = h.pointer;
    memset(use, 0, sizeof(*use));
    use->base_name = base_name;
    if (!loc.file && diag_get_file()) loc.file = ast_strdup(diag_get_file(), strlen(diag_get_file()));
    use->source_loc = loc;
    return use;
}

boolean_t ast_generic_arg_push(generic_use_t *use, type_info_t arg) {
    if (use->count >= SIZE_MAX / sizeof(type_info_t) ||
        (use->count == use->capacity && use->capacity > SIZE_MAX / sizeof(type_info_t) / 2)) {
        diag_begin_error("generic argument metadata allocation limit exceeded");
        diag_span(SRC_LOC(use->source_loc.line, use->source_loc.col, use->source_loc.len),
                  True, "application starts here");
        diag_finish();
        return False;
    }
    if (use->count == use->capacity) {
        use->capacity = use->capacity ? use->capacity * 2 : 4;
        if (!use->args_heap.pointer) {
            use->args_heap = allocate(use->capacity, sizeof(type_info_t));
            arena_track(use->args_heap);
        } else {
            use->args_heap = reallocate(use->args_heap,
                                        use->capacity * sizeof(type_info_t));
        }
        use->args = use->args_heap.pointer;
    }
    use->args[use->count++] = arg;
    return True;
}

/* ── escape-processing string copy ── */

static char decode_escape(char c) {
    switch (c) {
        case 'n':  return '\n';
        case 't':  return '\t';
        case 'r':  return '\r';
        case '0':  return '\0';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        case 'a':  return '\a';
        case 'b':  return '\b';
        case 'f':  return '\f';
        case 'v':  return '\v';
        default:   return c;
    }
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

char *ast_strdup_escape(const char *src, usize_t len, usize_t *out_len) {
    /* worst case: no expansion, same length */
    heap_t h = allocate(len + 1, sizeof(char));
    arena_track(h);
    char *dst = h.pointer;
    usize_t j = 0;

    for (usize_t i = 0; i < len; i++) {
        if (src[i] == '\\' && i + 1 < len) {
            i++;
            if (src[i] == 'x' && i + 2 < len) {
                int hi = hex_val(src[i + 1]);
                int lo = hex_val(src[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    dst[j++] = (char)(hi * 16 + lo);
                    i += 2;
                    continue;
                }
            }
            dst[j++] = decode_escape(src[i]);
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
    if (out_len) *out_len = j;
    return dst;
}

/* ── type array allocation ── */

type_info_t *alloc_type_array(usize_t count) {
    heap_t h = allocate(count, sizeof(type_info_t));
    arena_track(h);
    type_info_t *arr = h.pointer;
    for (usize_t i = 0; i < count; i++)
        arr[i] = NO_TYPE;
    return arr;
}

/* ── function pointer descriptor allocation ── */

fn_ptr_desc_t *alloc_fn_ptr_desc(usize_t param_count) {
    heap_t h = allocate(1, sizeof(fn_ptr_desc_t));
    arena_track(h);
    fn_ptr_desc_t *desc = h.pointer;
    desc->params = Null;
    desc->param_count = param_count;
    desc->ret_type = NO_TYPE;
    desc->is_c_abi = False;
    desc->is_variadic = False;
    if (param_count > 0) {
        heap_t ph = allocate(param_count, sizeof(fn_ptr_param_t));
        arena_track(ph);
        desc->params = ph.pointer;
        for (usize_t i = 0; i < param_count; i++) {
            desc->params[i].storage = StorageDefault;
            desc->params[i].type = NO_TYPE;
        }
    }
    return desc;
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

/* ── fileheader helpers ── */

void fileheader_init(fileheader_t *fh) {
    fh->items = Null;
    fh->count = 0;
    fh->capacity = 0;
    fh->heap = NullHeap;
}

void fileheader_push(fileheader_t *fh, fh_entry_t entry) {
    if (fh->count >= fh->capacity) {
        usize_t new_cap = fh->capacity < 4 ? 4 : fh->capacity * 2;
        if (fh->heap.pointer == Null) {
            fh->heap = allocate(new_cap, sizeof(fh_entry_t));
            arena_track(fh->heap);
        } else {
            fh->heap = reallocate(fh->heap, new_cap * sizeof(fh_entry_t));
        }
        fh->items = fh->heap.pointer;
        fh->capacity = new_cap;
    }
    fh->items[fh->count++] = entry;
}

fileheader_t *fileheader_alloc(void) {
    heap_t h = allocate(1, sizeof(fileheader_t));
    arena_track(h);
    fileheader_t *fh = h.pointer;
    fileheader_init(fh);
    return fh;
}

void fileheader_merge(fileheader_t *dst, fileheader_t *src) {
    if (!dst || !src) return;
    for (usize_t i = 0; i < src->count; i++)
        fileheader_push(dst, src->items[i]);
}
