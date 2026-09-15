/*
 * cbox_telemetry — native operation timing.
 *
 * Aggregates by operation type rather than keeping one record per call: the
 * consumer turns these into span tallies and a histogram, and an unbounded
 * list of records would be both a memory risk and a cardinality risk. The
 * individual calls still show up in the crash breadcrumb ring.
 */
#ifndef CBOX_OPS_H
#define CBOX_OPS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum _cbox_op_type {
	CBOX_OP_NONE = 0,
	CBOX_OP_PDO_CONNECT,
	CBOX_OP_REDIS_CONNECT,
	CBOX_OP_REDIS_PCONNECT,
	CBOX_OP_CURL_EXEC,
	CBOX_OP_STREAM_CONNECT,
	CBOX_OP_MAX
} cbox_op_type;

#define CBOX_OP_STACK_MAX 8u

typedef struct _cbox_op_agg {
	uint32_t count;
	uint64_t total_ns;
	uint64_t max_ns;
} cbox_op_agg;

typedef struct _cbox_op_frame {
	cbox_op_type type;
	uint64_t     start_ns;
} cbox_op_frame;

typedef struct _cbox_op_state {
	cbox_op_agg   agg[CBOX_OP_MAX];
	cbox_op_frame stack[CBOX_OP_STACK_MAX];
	uint32_t      depth;
	uint32_t      overflow; /* begins dropped because the nesting stack was full */
} cbox_op_state;

/* "pdo.connect", "curl.exec", … — stable wire names, safe to key on. */
const char *cbox_op_name(cbox_op_type type);

void cbox_ops_reset(cbox_op_state *state);
void cbox_ops_begin(cbox_op_state *state, cbox_op_type type, uint64_t now_ns);
void cbox_ops_end(cbox_op_state *state, cbox_op_type type, uint64_t now_ns);

/* The innermost in-flight operation, for crash context. CBOX_OP_NONE when idle. */
cbox_op_type cbox_ops_current(const cbox_op_state *state);
uint64_t     cbox_ops_current_start(const cbox_op_state *state);

#endif /* CBOX_OPS_H */
