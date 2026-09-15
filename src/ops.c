#include "ops.h"

#include <string.h>

const char *cbox_op_name(cbox_op_type type)
{
	switch (type) {
		case CBOX_OP_PDO_CONNECT:    return "pdo.connect";
		case CBOX_OP_REDIS_CONNECT:  return "redis.connect";
		case CBOX_OP_REDIS_PCONNECT: return "redis.pconnect";
		case CBOX_OP_CURL_EXEC:      return "curl.exec";
		case CBOX_OP_STREAM_CONNECT: return "stream.connect";
		case CBOX_OP_NONE:
		case CBOX_OP_MAX:
		default:                     return "";
	}
}

void cbox_ops_reset(cbox_op_state *state)
{
	memset(state, 0, sizeof(*state));
}

void cbox_ops_begin(cbox_op_state *state, cbox_op_type type, uint64_t now_ns)
{
	if (type <= CBOX_OP_NONE || type >= CBOX_OP_MAX) {
		return;
	}

	if (state->depth >= CBOX_OP_STACK_MAX) {
		state->overflow++;
		return;
	}

	state->stack[state->depth].type = type;
	state->stack[state->depth].start_ns = now_ns;
	state->depth++;
}

void cbox_ops_end(cbox_op_state *state, cbox_op_type type, uint64_t now_ns)
{
	cbox_op_frame *frame;
	uint64_t elapsed;
	cbox_op_agg *agg;

	if (type <= CBOX_OP_NONE || type >= CBOX_OP_MAX || state->depth == 0) {
		return;
	}

	frame = &state->stack[state->depth - 1];

	/*
	 * A mismatched end means a begin was dropped (stack overflow) or the call
	 * unwound through a bailout. Drop it rather than guess which frame it
	 * belonged to.
	 */
	if (frame->type != type) {
		return;
	}

	state->depth--;
	elapsed = now_ns > frame->start_ns ? now_ns - frame->start_ns : 0;

	agg = &state->agg[type];
	agg->count++;
	agg->total_ns += elapsed;

	if (elapsed > agg->max_ns) {
		agg->max_ns = elapsed;
	}
}

cbox_op_type cbox_ops_current(const cbox_op_state *state)
{
	return state->depth == 0 ? CBOX_OP_NONE : state->stack[state->depth - 1].type;
}

uint64_t cbox_ops_current_start(const cbox_op_state *state)
{
	return state->depth == 0 ? 0 : state->stack[state->depth - 1].start_ns;
}
