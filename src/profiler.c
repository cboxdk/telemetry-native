#include "php.h"
#include "zend_execute.h"

#include "clock.h"
#include "profiler.h"
#include "timer.h"

#include <signal.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
# define CBOX_ATOMIC_TAKE(target) __atomic_exchange_n((target), 0, __ATOMIC_RELAXED)
# define CBOX_ATOMIC_ADD(target, n) __atomic_fetch_add((target), (n), __ATOMIC_RELAXED)
#else
# define CBOX_ATOMIC_TAKE(target) (*(target)); *(target) = 0
# define CBOX_ATOMIC_ADD(target, n) (*(target) += (n))
#endif

/*
 * Signal-context state lives in file statics, not module globals: resolving a
 * TSRM cache from a signal handler is not async-signal-safe. Under NTS these
 * are per-process and therefore per-thread; a future ZTS build has to move the
 * non-signal half into module globals and keep this counter per-thread.
 */
static volatile uint32_t cbox_pending_ticks = 0;

static struct {
	cbox_arena       arena;
	cbox_frame_table frames;
	cbox_stack_tree  tree;

	uint64_t period_ns;
	uint64_t samples;
	uint64_t dropped;
	uint64_t deadline_ns; /* 0 = no limit */
	uint32_t max_depth;
	uint32_t truncated_frame;

	bool ready;
	bool running;
	bool installed;
	bool capped;
} cbox_profiler;

static void (*cbox_previous_interrupt)(zend_execute_data *execute_data) = NULL;

/* ------------------------------------------------------------ signal context */

void cbox_profiler_tick(uint32_t ticks)
{
	CBOX_ATOMIC_ADD(&cbox_pending_ticks, ticks == 0 ? 1 : ticks);
	zend_atomic_bool_store(&EG(vm_interrupt), 1);
}

/* ---------------------------------------------------------------- stack walk */

typedef struct _cbox_name_builder {
	char   buffer[CBOX_FRAME_NAME_MAX + 1];
	size_t length;
} cbox_name_builder;

/*
 * Scratch space for one sample, deliberately static rather than automatic.
 *
 * The interrupt handler runs on the VM's own C stack, at whatever depth the
 * engine happens to be — inside JIT'd code, under nested internal calls, close
 * to the stack limit PHP enforces. Putting ~1.4 KB of frame ids and name
 * building there on every sample is borrowing from a budget we did not set and
 * cannot see.
 *
 * Static is safe here because collection is not reentrant: it runs only from
 * zend_interrupt_function, at a VM safe point, and no PHP executes inside it.
 */
static uint32_t          cbox_sample_ids[CBOX_PROFILER_DEPTH_HARD_MAX];
static cbox_name_builder cbox_sample_name;

static void cbox_name_append(cbox_name_builder *name, const char *bytes, size_t len)
{
	size_t room;

	if (bytes == NULL || len == 0 || name->length >= CBOX_FRAME_NAME_MAX) {
		return;
	}

	room = CBOX_FRAME_NAME_MAX - name->length;

	if (len > room) {
		len = room;
	}

	memcpy(name->buffer + name->length, bytes, len);
	name->length += len;
}

static uint32_t cbox_profiler_frame_for(const zend_execute_data *frame)
{
	zend_function *func = frame->func;
	cbox_name_builder *name = &cbox_sample_name;
	const char *file = NULL;
	size_t file_len = 0;
	uint32_t line = 0;

	name->length = 0;

	if (func->common.scope != NULL && func->common.scope->name != NULL) {
		cbox_name_append(name, ZSTR_VAL(func->common.scope->name), ZSTR_LEN(func->common.scope->name));
		cbox_name_append(name, "::", 2);
	}

	if (func->common.function_name != NULL) {
		cbox_name_append(name, ZSTR_VAL(func->common.function_name), ZSTR_LEN(func->common.function_name));
	} else {
		/* File-level code has no function name; convention calls it {main}. */
		cbox_name_append(name, "{main}", 6);
	}

	/*
	 * The *declaration* site, not the currently executing line. Sample lines
	 * change constantly and would explode the frame table, and under tracing
	 * JIT the current opline can be stale — line_start never is.
	 */
	if (ZEND_USER_CODE(func->type)) {
		if (func->op_array.filename != NULL) {
			file = ZSTR_VAL(func->op_array.filename);
			file_len = ZSTR_LEN(func->op_array.filename);
		}

		line = func->op_array.line_start;
	}

	return cbox_frames_intern(
		&cbox_profiler.frames,
		&cbox_profiler.arena,
		name->buffer, name->length,
		file, file_len,
		line
	);
}

static uint32_t cbox_profiler_truncation_frame(void)
{
	if (cbox_profiler.truncated_frame != CBOX_FRAME_NONE) {
		return cbox_profiler.truncated_frame;
	}

	cbox_profiler.truncated_frame = cbox_frames_intern(
		&cbox_profiler.frames,
		&cbox_profiler.arena,
		"<truncated>", 11,
		NULL, 0,
		0
	);

	return cbox_profiler.truncated_frame;
}

static void cbox_profiler_collect(uint32_t weight)
{
	uint32_t *ids = cbox_sample_ids;
	uint32_t depth = 0;
	uint32_t node = CBOX_NODE_ROOT;
	uint32_t index;
	uint32_t max_depth = cbox_profiler.max_depth;
	bool truncated = false;
	zend_execute_data *frame = EG(current_execute_data);

	/* Defensive: never index the scratch array from unvalidated state. */
	if (max_depth == 0 || max_depth > CBOX_PROFILER_DEPTH_HARD_MAX) {
		max_depth = CBOX_PROFILER_DEPTH_HARD_MAX;
	}

	/* Innermost first, which is the order the engine keeps them in. */
	while (frame != NULL) {
		if (frame->func != NULL) {
			uint32_t id;

			if (depth >= max_depth) {
				truncated = true;
				break;
			}

			id = cbox_profiler_frame_for(frame);

			if (id == CBOX_FRAME_NONE) {
				cbox_profiler.dropped += weight;
				return;
			}

			ids[depth++] = id;
		}

		/*
		 * Inside a Fiber this chain ends at the fiber's own root rather than
		 * at the main stack, which is what we want: the sample describes the
		 * fiber that was actually on CPU.
		 */
		frame = frame->prev_execute_data;
	}

	if (truncated) {
		uint32_t marker = cbox_profiler_truncation_frame();

		if (marker != CBOX_FRAME_NONE) {
			node = cbox_stacktree_child(&cbox_profiler.tree, node, marker);

			if (node == CBOX_NODE_NONE) {
				cbox_profiler.dropped += weight;
				return;
			}
		}
	}

	/* Descend outermost to innermost so the trie mirrors the call path. */
	for (index = depth; index-- > 0;) {
		node = cbox_stacktree_child(&cbox_profiler.tree, node, ids[index]);

		if (node == CBOX_NODE_NONE) {
			cbox_profiler.dropped += weight;
			return;
		}
	}

	cbox_stacktree_record(&cbox_profiler.tree, node, weight);
	cbox_profiler.samples += weight;
}

static void cbox_profiler_interrupt(zend_execute_data *execute_data)
{
	uint32_t pending = CBOX_ATOMIC_TAKE(&cbox_pending_ticks);

	if (pending > 0 && cbox_profiler.running && cbox_profiler.ready) {
		cbox_profiler_collect(pending);

		/*
		 * Check the deadline rarely — once every few hundred samples is
		 * plenty for a valve measured in seconds, and a clock read on every
		 * sample would be a real cost at a 100 us period.
		 */
		if (cbox_profiler.deadline_ns != 0
			&& (cbox_profiler.samples & 0x3ff) == 0
			&& cbox_now_ns() > cbox_profiler.deadline_ns
		) {
			cbox_profiler_stop();
			cbox_profiler.capped = true;
		}
	}

	if (cbox_previous_interrupt != NULL) {
		cbox_previous_interrupt(execute_data);
	}
}

/* ------------------------------------------------------------------ lifecycle */

int cbox_profiler_init(uint32_t max_frames, uint32_t max_nodes, size_t arena_bytes)
{
	if (cbox_profiler.ready) {
		return 0;
	}

	memset(&cbox_profiler, 0, sizeof(cbox_profiler));
	cbox_profiler.truncated_frame = CBOX_FRAME_NONE;

	if (cbox_arena_init(&cbox_profiler.arena, arena_bytes) != 0) {
		return -1;
	}

	if (cbox_frames_init(&cbox_profiler.frames, max_frames) != 0) {
		cbox_arena_destroy(&cbox_profiler.arena);
		return -1;
	}

	if (cbox_stacktree_init(&cbox_profiler.tree, max_nodes) != 0) {
		cbox_frames_destroy(&cbox_profiler.frames);
		cbox_arena_destroy(&cbox_profiler.arena);
		return -1;
	}

	cbox_profiler.ready = true;

	return 0;
}

void cbox_profiler_shutdown(void)
{
	cbox_profiler_stop();
	cbox_stacktree_destroy(&cbox_profiler.tree);
	cbox_frames_destroy(&cbox_profiler.frames);
	cbox_arena_destroy(&cbox_profiler.arena);
	cbox_profiler.ready = false;
}

bool cbox_profiler_ready(void)
{
	return cbox_profiler.ready;
}

void cbox_profiler_install(void)
{
	if (cbox_profiler.installed) {
		return;
	}

	cbox_previous_interrupt = zend_interrupt_function;
	zend_interrupt_function = cbox_profiler_interrupt;
	cbox_profiler.installed = true;
}

void cbox_profiler_uninstall(void)
{
	if (!cbox_profiler.installed) {
		return;
	}

	/*
	 * Only unhook when nobody chained on top of us; otherwise we would drop
	 * their handler on the floor and break whoever came second.
	 */
	if (zend_interrupt_function == cbox_profiler_interrupt) {
		zend_interrupt_function = cbox_previous_interrupt;
	}

	cbox_previous_interrupt = NULL;
	cbox_profiler.installed = false;
}

int cbox_profiler_start(uint64_t period_ns, uint32_t max_depth, uint64_t max_duration_ns)
{
	if (!cbox_profiler.ready || period_ns == 0) {
		return -1;
	}

	if (max_depth == 0 || max_depth > CBOX_PROFILER_DEPTH_HARD_MAX) {
		max_depth = CBOX_PROFILER_DEPTH_HARD_MAX;
	}

	cbox_profiler_reset();
	cbox_profiler.max_depth = max_depth;
	cbox_profiler.period_ns = period_ns;
	cbox_profiler.deadline_ns = max_duration_ns == 0
		? 0
		: cbox_now_ns() + max_duration_ns;

	if (cbox_timer_arm(period_ns) != 0) {
		cbox_profiler.period_ns = 0;
		return -1;
	}

	cbox_profiler.running = true;

	return 0;
}

void cbox_profiler_stop(void)
{
	if (!cbox_profiler.running) {
		return;
	}

	cbox_timer_disarm();
	cbox_profiler.running = false;

	/* Anything the timer queued but the VM never got to is not a sample. */
	CBOX_ATOMIC_TAKE(&cbox_pending_ticks);
}

void cbox_profiler_reset(void)
{
	if (!cbox_profiler.ready) {
		return;
	}

	cbox_arena_reset(&cbox_profiler.arena);
	cbox_frames_reset(&cbox_profiler.frames);
	cbox_stacktree_reset(&cbox_profiler.tree);

	cbox_profiler.samples = 0;
	cbox_profiler.dropped = 0;
	cbox_profiler.truncated_frame = CBOX_FRAME_NONE;
	cbox_profiler.capped = false;
}

bool cbox_profiler_running(void)
{
	return cbox_profiler.running;
}

uint64_t cbox_profiler_period_ns(void)
{
	return cbox_profiler.period_ns;
}

uint64_t cbox_profiler_sample_count(void)
{
	return cbox_profiler.samples;
}

uint64_t cbox_profiler_dropped(void)
{
	return cbox_profiler.dropped + cbox_profiler.tree.dropped + cbox_profiler.frames.dropped;
}

bool cbox_profiler_capped(void)
{
	return cbox_profiler.capped;
}

size_t cbox_profiler_arena_peak(void)
{
	return cbox_profiler.arena.peak;
}

const cbox_frame_table *cbox_profiler_frames(void)
{
	return &cbox_profiler.frames;
}

const cbox_stack_tree *cbox_profiler_tree(void)
{
	return &cbox_profiler.tree;
}

const cbox_arena *cbox_profiler_arena(void)
{
	return &cbox_profiler.arena;
}
