/*
 * cbox_telemetry — unit of work.
 *
 * A unit is whatever the caller says it is: an HTTP request, a queue job, an
 * Artisan command, a scheduled task. The extension has no opinion about which
 * is which beyond a label, and knows nothing about Laravel.
 *
 * Trace and span ids are stored as raw bytes, not strings, so the crash record
 * can carry them at a fixed width.
 */
#ifndef CBOX_UNIT_H
#define CBOX_UNIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum _cbox_unit_type {
	CBOX_UNIT_NONE = 0,
	CBOX_UNIT_HTTP,
	CBOX_UNIT_QUEUE,
	CBOX_UNIT_COMMAND,
	CBOX_UNIT_SCHEDULE,
	CBOX_UNIT_OTHER,
	CBOX_UNIT_MAX
} cbox_unit_type;

#define CBOX_TRACE_ID_BYTES 16u
#define CBOX_SPAN_ID_BYTES   8u

typedef struct _cbox_unit_state {
	uint32_t handle;    /* 0 when no unit is active */
	uint8_t  type;
	bool     sampled;
	bool     profiling;
	bool     has_trace;

	/*
	 * An automatic unit was opened by the engine at RINIT rather than by a
	 * caller, so that measurement starts at the first instruction of the
	 * request instead of whenever the framework gets around to asking. The
	 * first begin() adopts it — keeping everything sampled so far — rather
	 * than throwing the bootstrap away and starting again.
	 */
	bool     automatic;
	bool     adopted;
	uint64_t start_ns;
	uint8_t  trace_id[CBOX_TRACE_ID_BYTES];
	uint8_t  span_id[CBOX_SPAN_ID_BYTES];
} cbox_unit_state;

const char     *cbox_unit_type_name(cbox_unit_type type);
cbox_unit_type  cbox_unit_type_from(const char *name, size_t len);

void cbox_unit_reset(cbox_unit_state *unit);

/* Strict: the whole input must be hex and exactly 2 * out_len long. */
bool cbox_hex_decode(const char *hex, size_t len, uint8_t *out, size_t out_len);

/* Writes 2 * len characters, no NUL. */
void cbox_hex_encode(const uint8_t *bytes, size_t len, char *out);

/* True when every byte is zero — an all-zero trace id is "absent", not valid. */
bool cbox_bytes_are_zero(const uint8_t *bytes, size_t len);

#endif /* CBOX_UNIT_H */
