/* ── symtab ── */

static void symtab_init(symtab_t *st) {
    st->entries = Null; st->count = 0; st->capacity = 0; st->heap = NullHeap;
}
static void symtab_free(symtab_t *st) {
    if (st->heap.pointer != Null) {
        deallocate(st->heap); st->heap = NullHeap;
        st->entries = Null; st->count = 0; st->capacity = 0;
    }
}
static void symtab_add(symtab_t *st, const char *name, LLVMValueRef value,
                        LLVMTypeRef type, type_info_t stype, int flags) {
    if (st->count >= st->capacity) {
        usize_t new_cap = st->capacity < 16 ? 16 : st->capacity * 2;
        if (st->heap.pointer == Null)
            st->heap = allocate(new_cap, sizeof(symbol_t));
        else
            st->heap = reallocate(st->heap, new_cap * sizeof(symbol_t));
        st->entries = st->heap.pointer;
        st->capacity = new_cap;
    }
    symbol_t sym = {0};
    sym.name = (char *)name;
    sym.value = value;
    sym.type = type;
    sym.stype = stype;
    sym.flags = flags;
    sym.array_size = -1; /* -1 = not an array / unknown size */
    st->entries[st->count++] = sym;
}
static symbol_t *symtab_lookup(symtab_t *st, const char *name) {
    for (usize_t i = st->count; i > 0; i--) {
        if (strcmp(st->entries[i - 1].name, name) == 0)
            return &st->entries[i - 1];
    }
    return Null;
}
static symbol_t *cg_lookup(cg_t *cg, const char *name) {
    symbol_t *s = symtab_lookup(&cg->locals, name);
    if (s) { s->used = True; return s; }
    return symtab_lookup(&cg->globals, name);
}

static void symtab_set_last_line(symtab_t *st, usize_t line) {
    if (st->count > 0)
        st->entries[st->count - 1].line = line;
}

static void symtab_set_last_storage(symtab_t *st, storage_t storage, boolean_t is_heap_var) {
    if (st->count > 0) {
        st->entries[st->count - 1].storage = storage;
        if (is_heap_var)
            st->entries[st->count - 1].flags |= SymHeapVar;
        else
            st->entries[st->count - 1].flags &= ~SymHeapVar;
    }
}

static void symtab_set_last_extra(symtab_t *st, boolean_t is_const, boolean_t is_final,
                                   linkage_t linkage, usize_t scope_depth, long array_size) {
    if (st->count > 0) {
        symbol_t *s = &st->entries[st->count - 1];
        if (is_const)  s->flags |= SymConst;  else s->flags &= ~SymConst;
        if (is_final)  s->flags |= SymFinal;  else s->flags &= ~SymFinal;
        s->linkage     = linkage;
        s->scope_depth = scope_depth;
        s->array_size  = array_size;
    }
}

static void symtab_set_last_nil(symtab_t *st, boolean_t is_nil) {
    if (st->count > 0) {
        if (is_nil) st->entries[st->count - 1].flags |= SymNil;
        else        st->entries[st->count - 1].flags &= ~SymNil;
    }
}
