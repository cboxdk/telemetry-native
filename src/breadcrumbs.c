#include "breadcrumbs.h"

#include <stdlib.h>
#include <string.h>

static uint32_t cbox_round_pow2(uint32_t value)
{
	uint32_t result = 16;

	while (result < value && result < (1u << 20)) {
		result <<= 1;
	}

	return result;
}

int cbox_crumbs_init(cbox_crumb_ring *ring, uint32_t capacity)
{
	memset(ring, 0, sizeof(*ring));

	if (capacity == 0) {
		return -1;
	}

	capacity = cbox_round_pow2(capacity);
	ring->entries = (cbox_crumb *) calloc(capacity, sizeof(cbox_crumb));

	if (ring->entries == NULL) {
		return -1;
	}

	ring->capacity = capacity;
	ring->mask = capacity - 1;

	return 0;
}

void cbox_crumbs_destroy(cbox_crumb_ring *ring)
{
	free(ring->entries);
	memset(ring, 0, sizeof(*ring));
}

void cbox_crumbs_reset(cbox_crumb_ring *ring)
{
	if (ring->entries == NULL) {
		return;
	}

	memset(ring->entries, 0, (size_t) ring->capacity * sizeof(cbox_crumb));
	ring->head = 0;
	ring->seq = 0;
	ring->written = 0;
}

void cbox_crumbs_push(
	cbox_crumb_ring *ring,
	cbox_crumb_type  type,
	uint8_t          flags,
	const char      *label,
	size_t           label_len,
	uint64_t         now_ns
) {
	cbox_crumb *entry;

	if (ring->entries == NULL) {
		return;
	}

	if (label == NULL) {
		label_len = 0;
	} else if (label_len > CBOX_CRUMB_LABEL_MAX) {
		label_len = CBOX_CRUMB_LABEL_MAX;
	}

	entry = &ring->entries[ring->head];

	/*
	 * Retract the slot before touching it. A crash between here and the
	 * publish below finds sequence zero and skips the entry rather than
	 * reporting a mixture of this operation and the one that used the slot
	 * before it.
	 */
	__atomic_store_n(&entry->seq, 0u, __ATOMIC_RELEASE);

	entry->ts_ns = now_ns;
	entry->type = (uint8_t) type;
	entry->flags = flags;
	entry->label_len = (uint16_t) label_len;

	if (label_len > 0) {
		memcpy(entry->label, label, label_len);
	}

	if (label_len < CBOX_CRUMB_LABEL_MAX) {
		memset(entry->label + label_len, 0, CBOX_CRUMB_LABEL_MAX - label_len);
	}

	/* Publish. Everything above is visible to a reader that sees this. */
	__atomic_store_n(&entry->seq, ++ring->seq, __ATOMIC_RELEASE);

	__atomic_store_n(&ring->head, (ring->head + 1) & ring->mask, __ATOMIC_RELEASE);
	ring->written++;
}

uint32_t cbox_crumbs_snapshot(const cbox_crumb_ring *ring, cbox_crumb *dst, uint32_t max)
{
	uint32_t available, count, i, index, copied = 0;
	uint32_t head;

	if (ring->entries == NULL || dst == NULL || max == 0) {
		return 0;
	}

	head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);

	available = ring->written > (uint64_t) ring->capacity
		? ring->capacity
		: (uint32_t) ring->written;

	count = available < max ? available : max;

	for (i = 0; i < count; i++) {
		/* head - count + i, wrapped: oldest of the retained window first. */
		const cbox_crumb *entry;
		uint32_t before, after;

		index = (head + ring->capacity - count + i) & ring->mask;
		entry = &ring->entries[index];

		before = __atomic_load_n(&entry->seq, __ATOMIC_ACQUIRE);

		if (before == 0) {
			continue; /* being written right now — incomplete, so not reported */
		}

		dst[copied] = *entry;

		after = __atomic_load_n(&entry->seq, __ATOMIC_ACQUIRE);

		/*
		 * Changed underneath us. No retry: this runs in a signal handler after
		 * something has already gone wrong, and an unbounded loop there is a
		 * worse failure than one missing breadcrumb.
		 */
		if (after != before) {
			continue;
		}

		copied++;
	}

	return copied;
}
