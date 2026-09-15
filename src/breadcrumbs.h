/*
 * cbox_telemetry — crash breadcrumbs.
 *
 * A fixed ring of fixed-width entries answering one question: what was PHP
 * doing immediately before it died?
 *
 * Every entry is exactly 64 bytes and the label is copied *at write time*,
 * truncated to fit. That copy is the whole point — after a SIGSEGV there is
 * no frame table left to resolve an id against, so anything the record needs
 * to be readable must already be inside it.
 *
 * Never store arguments, SQL, URLs, keys or any other application value here.
 * Labels are function and operation names only.
 */
#ifndef CBOX_BREADCRUMBS_H
#define CBOX_BREADCRUMBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CBOX_CRUMB_LABEL_MAX 48u

typedef enum _cbox_crumb_type {
	CBOX_CRUMB_NONE = 0,
	CBOX_CRUMB_UNIT_BEGIN,
	CBOX_CRUMB_UNIT_END,
	CBOX_CRUMB_OP_BEGIN,
	CBOX_CRUMB_OP_END,
	CBOX_CRUMB_GC
} cbox_crumb_type;

typedef struct _cbox_crumb {
	uint64_t ts_ns;
	uint32_t seq;
	uint8_t  type;
	uint8_t  flags;
	uint16_t label_len;
	char     label[CBOX_CRUMB_LABEL_MAX];
} cbox_crumb;

typedef struct _cbox_crumb_ring {
	cbox_crumb *entries;
	uint32_t    capacity; /* always a power of two */
	uint32_t    mask;
	uint32_t    head;     /* index of the next write */
	uint32_t    seq;
	uint64_t    written;
} cbox_crumb_ring;

int  cbox_crumbs_init(cbox_crumb_ring *ring, uint32_t capacity);
void cbox_crumbs_destroy(cbox_crumb_ring *ring);
void cbox_crumbs_reset(cbox_crumb_ring *ring);

void cbox_crumbs_push(
	cbox_crumb_ring *ring,
	cbox_crumb_type  type,
	uint8_t          flags,
	const char      *label,
	size_t           label_len,
	uint64_t         now_ns
);

/*
 * Copy up to `max` of the most recent entries into `dst`, oldest first.
 * Signal-safe: no allocation, no locks, plain copies out of the ring.
 */
uint32_t cbox_crumbs_snapshot(const cbox_crumb_ring *ring, cbox_crumb *dst, uint32_t max);

#endif /* CBOX_BREADCRUMBS_H */
