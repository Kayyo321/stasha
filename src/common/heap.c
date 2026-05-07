static void ensure_heap_list_integrity(void) {
    if (!heap_list_head) {
        heap_list_head = (heap_list_t *)malloc(sizeof(heap_list_t));
        if (!heap_list_head) {
            log_err("Failed to allocate memory for heap list head.");
            quit(Err);
        }
        heap_list_head->heap = NullHeap;
        heap_list_head->next = Null;
    }

    // configure last pointer if it's not set
    if (!heap_list_last) {
        heap_list_t *current = heap_list_head;
        while (current->next) {
            current = current->next;
        }
        heap_list_last = current;
    }
}

static heap_list_t *get_new_node(void) {
    ensure_heap_list_integrity();

    // change the heap_last to point to a new node, then move the heap_last to the new nodes next pointer
    heap_list_t *new_node = (heap_list_t *)malloc(sizeof(heap_list_t));
    if (!new_node) {
        log_err("Failed to allocate memory for new heap node.");
        quit(Err);
    }
    new_node->heap = NullHeap;
    new_node->next = Null;
    new_node->prev_next = &heap_list_last->next;

    heap_list_last->next = new_node;
    heap_list_last = new_node;

    return new_node;
}

heap_t allocate(usize_t count, usize_t bytes) {
    heap_list_t *node = get_new_node();

    const usize_t total_size = count * bytes;

    node->heap.pointer = malloc(total_size);
    node->heap.size = total_size;
    if (!node->heap.pointer) {
        log_err("Failed to allocate memory: requested %lu bytes.", total_size);
        quit(Err);
    }

    node->heap.priv = node; // private data is a pointer to the node itself, so we can find it when we need to reallocate or deallocate

    return node->heap;
}

heap_t reallocate(heap_t heap, usize_t new_size) {
    if (!heap.pointer) {
        log_err("Cannot reallocate a null pointer.");
        quit(Err);
    }

    heap_list_t *node = (heap_list_t *)heap.priv;
    if (!node) {
        log_err("Invalid heap provided for reallocation.");
        quit(Err);
    }

    void *new_ptr = realloc(node->heap.pointer, new_size);
    if (!new_ptr) {
        log_err("Failed to reallocate memory: requested %lu bytes.", new_size);
        quit(Err);
    }

    node->heap.pointer = new_ptr;
    node->heap.size = new_size;

    return node->heap;
}

static void wipe_memory(void *pointer, usize_t size) {
    #if !defined(Debug) && !defined(Testing)
        memset(pointer, 0, size); // 0 out all allocations on release builds
    #endif
}

void deallocate(heap_t heap) {
    // deallocate the heap and remove it from the list
    if (!heap.pointer) {
        log_err("Cannot deallocate a null pointer.");
        quit(Err);
    }

    heap_list_t *node = (heap_list_t *)heap.priv;
    if (!node) {
        log_err("Invalid heap provided for deallocation.");
        quit(Err);
    }

    wipe_memory(node->heap.pointer, node->heap.size);

    free(node->heap.pointer);
    node->heap.pointer = Null;
    node->heap.size = 0;
    node->heap.priv = Null;

    if (!node->prev_next || *node->prev_next != node) {
        log_err("Heap list corruption detected during deallocation.");
        quit(Err);
    }

    *node->prev_next = node->next;
    if (node->next) {
        node->next->prev_next = node->prev_next;
    } else if (node->prev_next == &heap_list_head->next) {
        heap_list_last = heap_list_head;
    } else {
        heap_list_t *prev = (heap_list_t *)((char *)node->prev_next - offsetof(heap_list_t, next));
        heap_list_last = prev;
    }

    free(node);
}

result_t scan_and_deallocate(void) {
    ensure_heap_list_integrity();

    heap_list_t *current = heap_list_head->next;
    usize_t reclaimed_bytes = 0;

    while (current) {
        heap_list_t *next = current->next;
        if (current->heap.pointer) {
            log_warn("Memory leak detected: %lu bytes at %p", current->heap.size, current->heap.pointer);
            reclaimed_bytes += current->heap.size;
            deallocate(current->heap);
        }
        current = next;
    }

    return (reclaimed_bytes > 0) ? Err : Ok;
}