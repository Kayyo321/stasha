/* ── Stasha Zone Runtime ─────────────────────────────────────────────────────
 *
 * Lightweight linked-list arena allocator for zone-based memory management.
 * Compiled separately and linked into every Stasha executable.
 *
 * A "zone" is a bump-allocator backed by a chain of fixed-size blocks.  All
 * allocations from a zone are freed at once when __zone_free() is called.
 *
 * API used by compiler-generated code:
 *
 *   void *__zone_alloc(void **zone_ptr, size_t size)
 *       Allocate `size` bytes from the zone.  *zone_ptr is a pointer to the
 *       zone's internal state (initially NULL; allocated on first use).
 *
 *   void __zone_free(void **zone_ptr)
 *       Free all memory allocated from the zone.  *zone_ptr is set to NULL.
 *       Safe to call on a NULL zone.
 *
 *   void *__zone_move(void **zone_ptr, void *ptr, size_t size)
 *       Copy `size` bytes from `ptr` (which must be inside the zone) to an
 *       independent malloc'd allocation.  Removes the pointer from zone
 *       provenance; the caller is responsible for free()ing the result.
 *       Returns the new heap pointer.
 * ── */

#ifndef STASHA_ZONE_RUNTIME_H
#define STASHA_ZONE_RUNTIME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate `size` bytes from the zone identified by *zone_ptr.
 * *zone_ptr is a void* that the runtime uses for internal state.
 * Pass a pointer to a NULL-initialised void* for a fresh zone. */
void *__zone_alloc(void **zone_ptr, size_t size);

/* Free all memory allocated from the zone and reset *zone_ptr to NULL.
 * Safe to call when *zone_ptr is NULL (no-op). */
void __zone_free(void **zone_ptr);

/* Copy `size` bytes from `ptr` (a zone allocation) to a fresh malloc().
 * The copy is NOT tracked by any zone.  The caller must free() the result. */
void *__zone_move(void **zone_ptr, void *ptr, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* STASHA_ZONE_RUNTIME_H */
