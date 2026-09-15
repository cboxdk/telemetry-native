/*
 * cbox_telemetry — bump arena.
 *
 * One malloc'd slab per process, handed out in bump order and reset in O(1)
 * between units of work. Used for the variable-length bytes the profiler
 * needs (function and file names); everything fixed-size lives in its own
 * preallocated array.
 *
 * Deliberately NOT the Zend allocator: the profiler appends to this from the
 * VM interrupt handler, it must survive across requests in a long-lived
 * worker, and it must never grow.
 */
#ifndef CBOX_ARENA_H
#define CBOX_ARENA_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct _cbox_arena {
	char   *base;
	size_t  size;
	size_t  used;
	size_t  peak;
	uint32_t exhausted; /* count of allocations refused since the last reset */
} cbox_arena;

/* 0 on success, -1 when the slab could not be allocated. */
int  cbox_arena_init(cbox_arena *arena, size_t size);
void cbox_arena_destroy(cbox_arena *arena);

/*
 * Returns NULL when the slab is full — callers must handle that by dropping
 * data and bumping a counter, never by allocating elsewhere.
 */
void *cbox_arena_alloc(cbox_arena *arena, size_t bytes);

/* Copies len bytes plus a NUL. Returns the offset from base, or CBOX_ARENA_NONE. */
#define CBOX_ARENA_NONE UINT32_MAX
uint32_t cbox_arena_put(cbox_arena *arena, const char *bytes, size_t len);

static inline const char *cbox_arena_at(const cbox_arena *arena, uint32_t offset)
{
	return offset == CBOX_ARENA_NONE ? NULL : arena->base + offset;
}

void cbox_arena_reset(cbox_arena *arena);

#endif /* CBOX_ARENA_H */
