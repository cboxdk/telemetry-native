#include "frames.h"

#include <stdlib.h>
#include <string.h>

static uint32_t cbox_fnv1a(const char *bytes, size_t len, uint32_t seed)
{
	uint32_t hash = seed;
	size_t i;

	for (i = 0; i < len; i++) {
		hash ^= (uint32_t) (unsigned char) bytes[i];
		hash *= 16777619u;
	}

	return hash;
}

/* Smallest power of two strictly greater than 2 * capacity, so load stays < 0.5. */
static uint32_t cbox_bucket_count(uint32_t capacity)
{
	uint32_t buckets = 16;

	while (buckets < capacity * 2u && buckets < (1u << 30)) {
		buckets <<= 1;
	}

	return buckets;
}

int cbox_frames_init(cbox_frame_table *table, uint32_t capacity)
{
	uint32_t buckets;

	memset(table, 0, sizeof(*table));

	if (capacity == 0) {
		return -1;
	}

	buckets = cbox_bucket_count(capacity);

	table->frames = (cbox_frame *) calloc(capacity, sizeof(cbox_frame));
	table->buckets = (uint32_t *) calloc(buckets, sizeof(uint32_t));

	if (table->frames == NULL || table->buckets == NULL) {
		cbox_frames_destroy(table);
		return -1;
	}

	table->capacity = capacity;
	table->bucket_mask = buckets - 1;

	return 0;
}

void cbox_frames_destroy(cbox_frame_table *table)
{
	free(table->frames);
	free(table->buckets);
	memset(table, 0, sizeof(*table));
}

void cbox_frames_reset(cbox_frame_table *table)
{
	if (table->count > 0) {
		memset(table->buckets, 0, ((size_t) table->bucket_mask + 1) * sizeof(uint32_t));
		table->count = 0;
	}

	table->capacity_hits = 0;
}

uint32_t cbox_frames_intern(
	cbox_frame_table *table,
	cbox_arena       *arena,
	const char       *name, size_t name_len,
	const char       *file, size_t file_len,
	uint32_t          line
) {
	uint32_t hash, slot, id;
	cbox_frame *frame;

	if (table->frames == NULL || name == NULL || name_len == 0) {
		return CBOX_FRAME_NONE;
	}

	if (name_len > CBOX_FRAME_NAME_MAX) {
		name_len = CBOX_FRAME_NAME_MAX;
	}

	if (file_len > CBOX_FRAME_NAME_MAX) {
		file_len = CBOX_FRAME_NAME_MAX;
	}

	hash = cbox_fnv1a(name, name_len, 2166136261u);

	if (file_len > 0) {
		hash = cbox_fnv1a(file, file_len, hash);
	}

	/*
	 * The declaration line is part of identity, not decoration. Before PHP 8.4
	 * every closure in a file is called "{closure}", so name+file alone folds
	 * all of them into one frame and reports their samples against whichever
	 * happened to be seen first.
	 */
	hash = cbox_fnv1a((const char *) &line, sizeof(line), hash);

	slot = hash & table->bucket_mask;

	while (table->buckets[slot] != 0) {
		id = table->buckets[slot] - 1;
		frame = &table->frames[id];

		if (frame->hash == hash
			&& frame->line == line
			&& frame->name_len == name_len
			&& memcmp(cbox_arena_at(arena, frame->name_off), name, name_len) == 0
			&& frame->file_len == file_len
			&& (file_len == 0
				|| memcmp(cbox_arena_at(arena, frame->file_off), file, file_len) == 0)
		) {
			return id;
		}

		slot = (slot + 1) & table->bucket_mask;
	}

	if (table->count >= table->capacity) {
		table->capacity_hits++;
		return CBOX_FRAME_NONE;
	}

	id = table->count;
	frame = &table->frames[id];
	frame->name_off = cbox_arena_put(arena, name, name_len);

	if (frame->name_off == CBOX_ARENA_NONE) {
		table->capacity_hits++;
		return CBOX_FRAME_NONE;
	}

	frame->file_off = file_len > 0
		? cbox_arena_put(arena, file, file_len)
		: CBOX_ARENA_NONE;

	if (file_len > 0 && frame->file_off == CBOX_ARENA_NONE) {
		table->capacity_hits++;
		return CBOX_FRAME_NONE;
	}

	frame->hash = hash;
	frame->name_len = (uint16_t) name_len;
	frame->file_len = (uint16_t) file_len;
	frame->line = line;

	table->count++;
	table->buckets[slot] = id + 1;

	return id;
}
