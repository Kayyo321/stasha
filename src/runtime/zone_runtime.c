/* ── Stasha Zone Runtime implementation ─────────────────────────────────── */

#include "zone_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdalign.h>

/* Each block in the zone chain.
 * Layout: [zone_block_t header] [data bytes ...] */
#define ZONE_BLOCK_SIZE (64u * 1024u)  /* 64 KiB per block */

typedef struct zone_block {
    struct zone_block *next;
    size_t             used;   /* bytes used in this block */
    size_t             cap;    /* usable bytes after the header */
    alignas(max_align_t) unsigned char data[];
} zone_block_t;

/* The zone handle stored in the user's void*. */
typedef struct zone_state {
    zone_block_t *head;   /* current (most-recent) block */
} zone_state_t;

/* ── internal helpers ── */

static zone_block_t *zone_new_block(size_t min_size) {
    size_t cap = min_size > ZONE_BLOCK_SIZE ? min_size : ZONE_BLOCK_SIZE;
    if (cap > SIZE_MAX - sizeof(zone_block_t)) return NULL;
    zone_block_t *blk = (zone_block_t *)malloc(sizeof(zone_block_t) + cap);
    if (!blk) return NULL;
    blk->next = NULL;
    blk->used = 0;
    blk->cap  = cap;
    return blk;
}

/* ── public API ── */

void *__zone_alloc(void **zone_ptr, size_t size) {
    if (!zone_ptr) return NULL;
    if (size == 0) size = 1;

    const size_t alignment = alignof(max_align_t);
    size_t remainder = size % alignment;
    size_t padding = remainder ? alignment - remainder : 0;
    if (size > SIZE_MAX - padding) return NULL;
    size_t aligned = size + padding;
    if (aligned > SIZE_MAX - sizeof(zone_block_t)) return NULL;

    /* Initialise zone on first use */
    zone_state_t *zs = (zone_state_t *)*zone_ptr;
    if (!zs) {
        zs = (zone_state_t *)malloc(sizeof(zone_state_t));
        if (!zs) return NULL;
        zs->head = NULL;
        *zone_ptr = zs;
    }

    /* Find a block with enough space or allocate a new one */
    zone_block_t *blk = zs->head;
    if (!blk || aligned > blk->cap - blk->used) {
        zone_block_t *new_blk = zone_new_block(aligned);
        if (!new_blk) return NULL;
        new_blk->next = blk;
        zs->head = new_blk;
        blk = new_blk;
    }

    void *ptr = blk->data + blk->used;
    blk->used += aligned;
    return ptr;
}

void __zone_free(void **zone_ptr) {
    if (!zone_ptr || !*zone_ptr) return;
    zone_state_t *zs = (zone_state_t *)*zone_ptr;

    /* Free all blocks in the chain */
    zone_block_t *blk = zs->head;
    while (blk) {
        zone_block_t *next = blk->next;
        free(blk);
        blk = next;
    }

    free(zs);
    *zone_ptr = NULL;
}

void *__zone_move(void **zone_ptr, void *ptr, size_t size) {
    /* Allocate an independent copy via malloc (not zone-backed). */
    (void)zone_ptr; /* zone_ptr unused — we don't need to remove from zone */
    if (!ptr || size == 0) return NULL;
    void *copy = malloc(size);
    if (!copy) return NULL;
    memcpy(copy, ptr, size);
    return copy;
}
