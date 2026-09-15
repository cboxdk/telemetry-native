#include "arena.h"

#include <stdlib.h>
#include <string.h>

#define CBOX_ARENA_ALIGN 8u

int cbox_arena_init(cbox_arena *arena, size_t size)
{
	memset(arena, 0, sizeof(*arena));

	if (size == 0) {
		return -1;
	}

	arena->base = (char *) malloc(size);

	if (arena->base == NULL) {
		return -1;
	}

	arena->size = size;

	return 0;
}

void cbox_arena_destroy(cbox_arena *arena)
{
	free(arena->base);
	memset(arena, 0, sizeof(*arena));
}

void *cbox_arena_alloc(cbox_arena *arena, size_t bytes)
{
	size_t aligned, offset;

	if (arena->base == NULL) {
		return NULL;
	}

	/* Round the *cursor* up, so every handout is 8-byte aligned. */
	offset = (arena->used + (CBOX_ARENA_ALIGN - 1)) & ~((size_t) CBOX_ARENA_ALIGN - 1);
	aligned = offset + bytes;

	if (aligned < offset || aligned > arena->size) { /* overflow or full */
		arena->exhausted++;
		return NULL;
	}

	arena->used = aligned;

	if (aligned > arena->peak) {
		arena->peak = aligned;
	}

	return arena->base + offset;
}

uint32_t cbox_arena_put(cbox_arena *arena, const char *bytes, size_t len)
{
	char *slot = (char *) cbox_arena_alloc(arena, len + 1);

	if (slot == NULL) {
		return CBOX_ARENA_NONE;
	}

	if (len > 0) {
		memcpy(slot, bytes, len);
	}

	slot[len] = '\0';

	return (uint32_t) (slot - arena->base);
}

void cbox_arena_reset(cbox_arena *arena)
{
	arena->used = 0;
	arena->exhausted = 0;
}
