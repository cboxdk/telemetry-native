/*
 * cbox_telemetry — frame interning.
 *
 * A frame is one *function*, not one call site: name, declaring file and
 * declaration line. Line numbers of individual samples deliberately do not
 * take part in frame identity — they change on every sample and would blow
 * the table up within a single request.
 *
 * Names are copied into the arena rather than referenced as zend_string*,
 * because a non-interned zend_string can be freed and its address reused
 * before the profile is materialised.
 */
#ifndef CBOX_FRAMES_H
#define CBOX_FRAMES_H

#include "arena.h"

#define CBOX_FRAME_NAME_MAX 320u
#define CBOX_FRAME_NONE     UINT32_MAX

typedef struct _cbox_frame {
	uint32_t hash;
	uint32_t name_off; /* arena offset, never CBOX_ARENA_NONE for a live frame */
	uint32_t file_off; /* arena offset, or CBOX_ARENA_NONE for internal functions */
	uint32_t line;     /* declaration line, 0 when unknown */
	uint16_t name_len;
	uint16_t file_len;
} cbox_frame;

typedef struct _cbox_frame_table {
	cbox_frame *frames;
	uint32_t    count;
	uint32_t    capacity;

	uint32_t   *buckets;     /* open addressing; 0 = empty, else frame_id + 1 */
	uint32_t    bucket_mask;

	uint32_t    capacity_hits; /* times a frame could not be interned */
} cbox_frame_table;

int  cbox_frames_init(cbox_frame_table *table, uint32_t capacity);
void cbox_frames_destroy(cbox_frame_table *table);
void cbox_frames_reset(cbox_frame_table *table);

/*
 * Returns the frame id for this identity, interning it on first sight.
 * CBOX_FRAME_NONE when the table or the arena is full.
 */
uint32_t cbox_frames_intern(
	cbox_frame_table *table,
	cbox_arena       *arena,
	const char       *name, size_t name_len,
	const char       *file, size_t file_len,
	uint32_t          line
);

#endif /* CBOX_FRAMES_H */
