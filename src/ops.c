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

cbox_op_token cbox_ops_begin(cbox_op_state *state, cbox_op_type type, uint64_t now_ns)
{
	cbox_op_token token;

	token.type = type;
	token.start_ns = now_ns;
	token.slot = CBOX_OP_NO_SLOT;

	if (type <= CBOX_OP_NONE || type >= CBOX_OP_MAX) {
		token.type = CBOX_OP_NONE;
		return token;
	}

	/*
	 * A slot is only needed so a crash record can say what was in flight. Not
	 * getting one costs that context, never the measurement — the token below
	 * is what the timing depends on.
	 */
	if (state->depth < CBOX_OP_STACK_MAX) {
		token.slot = state->depth;
		state->stack[state->depth].type = type;
		state->stack[state->depth].start_ns = now_ns;
		state->depth++;
	} else {
		state->overflow++;
	}

	return token;
}

void cbox_ops_end(cbox_op_state *state, cbox_op_token token, uint64_t now_ns)
{
	uint64_t elapsed;
	cbox_op_agg *agg;

	if (token.type <= CBOX_OP_NONE || token.type >= CBOX_OP_MAX) {
		return;
	}

	if (token.slot != CBOX_OP_NO_SLOT && token.slot < CBOX_OP_STACK_MAX) {
		/*
		 * Release exactly this slot. Operations can finish out of order — two
		 * Fibers interleaving — so a slot that is not the top becomes a hole
		 * and cbox_ops_current() skips it.
		 */
		state->stack[token.slot].type = CBOX_OP_NONE;

		while (state->depth > 0 && state->stack[state->depth - 1].type == CBOX_OP_NONE) {
			state->depth--;
		}
	}

	elapsed = now_ns > token.start_ns ? now_ns - token.start_ns : 0;

	agg = &state->agg[token.type];
	agg->count++;
	agg->total_ns += elapsed;

	if (elapsed > agg->max_ns) {
		agg->max_ns = elapsed;
	}
}

/* Innermost slot still occupied, skipping holes left by out-of-order ends. */
static uint32_t cbox_ops_top(const cbox_op_state *state)
{
	uint32_t index = state->depth;

	while (index > 0) {
		if (state->stack[index - 1].type != CBOX_OP_NONE) {
			return index;
		}

		index--;
	}

	return 0;
}

cbox_op_type cbox_ops_current(const cbox_op_state *state)
{
	uint32_t top = cbox_ops_top(state);

	return top == 0 ? CBOX_OP_NONE : state->stack[top - 1].type;
}

uint64_t cbox_ops_current_start(const cbox_op_state *state)
{
	uint32_t top = cbox_ops_top(state);

	return top == 0 ? 0 : state->stack[top - 1].start_ns;
}
