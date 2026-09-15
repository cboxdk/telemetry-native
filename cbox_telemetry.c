#include "php_cbox_telemetry.h"

#include "php_ini.h"
#include "SAPI.h"
#include "ext/standard/info.h"
#include "zend_gc.h"

#include "cbox_telemetry_arginfo.h"

#include "src/clock.h"
#include "src/crash.h"
#include "src/hooks.h"
#include "src/profiler.h"
#include "src/sigstack.h"
#include "src/timer.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

ZEND_DECLARE_MODULE_GLOBALS(cbox_telemetry)

/* --------------------------------------------------------------------- utils */

static zend_long cbox_clamp(zend_long value, zend_long min, zend_long max)
{
	if (value < min) {
		return min;
	}

	return value > max ? max : value;
}

static const char *cbox_signal_name(uint32_t signal)
{
	switch (signal) {
		case SIGSEGV: return "SIGSEGV";
		case SIGABRT: return "SIGABRT";
		case SIGBUS:  return "SIGBUS";
		case SIGILL:  return "SIGILL";
		default:      return "UNKNOWN";
	}
}

static const char *cbox_crumb_type_name(uint8_t type)
{
	switch (type) {
		case CBOX_CRUMB_UNIT_BEGIN: return "unit.begin";
		case CBOX_CRUMB_UNIT_END:   return "unit.end";
		case CBOX_CRUMB_OP_BEGIN:   return "op.begin";
		case CBOX_CRUMB_OP_END:     return "op.end";
		case CBOX_CRUMB_GC:         return "gc";
		default:                    return "none";
	}
}

static void cbox_gc_counters(uint64_t *runs, uint64_t *collected)
{
	zend_gc_status status;

	zend_gc_get_status(&status);

	*runs = (uint64_t) status.runs;
	*collected = (uint64_t) status.collected;
}

/* --------------------------------------------------- operation notifications */

void cbox_telemetry_note_op_begin(cbox_op_type type)
{
	uint64_t now = cbox_now_ns();
	const char *name = cbox_op_name(type);

	/*
	 * Breadcrumbs are recorded whether or not a unit is active — a crash
	 * outside a unit still deserves context. Aggregates are not: they belong
	 * to a unit, and a worker sitting idle must not accumulate them.
	 */
	cbox_crumbs_push(&CBOX_G(crumbs), CBOX_CRUMB_OP_BEGIN, (uint8_t) type, name, strlen(name), now);

	if (CBOX_G(unit).handle != 0) {
		cbox_ops_begin(&CBOX_G(ops), type, now);
	}
}

void cbox_telemetry_note_op_end(cbox_op_type type)
{
	uint64_t now = cbox_now_ns();
	const char *name = cbox_op_name(type);

	if (CBOX_G(unit).handle != 0) {
		cbox_ops_end(&CBOX_G(ops), type, now);
	}

	cbox_crumbs_push(&CBOX_G(crumbs), CBOX_CRUMB_OP_END, (uint8_t) type, name, strlen(name), now);
}

/* ------------------------------------------------------------- profile output */

typedef struct _cbox_function_total {
	uint32_t frame_id;
	uint64_t samples;
} cbox_function_total;

static int cbox_function_total_cmp(const void *left, const void *right)
{
	const cbox_function_total *a = (const cbox_function_total *) left;
	const cbox_function_total *b = (const cbox_function_total *) right;

	if (a->samples != b->samples) {
		return a->samples < b->samples ? 1 : -1; /* descending */
	}

	/* Stable-ish tiebreak so output does not shuffle between identical runs. */
	return a->frame_id < b->frame_id ? -1 : (a->frame_id > b->frame_id ? 1 : 0);
}

static void cbox_profile_into(zval *profile, bool include_stacks)
{
	const cbox_frame_table *frames = cbox_profiler_frames();
	const cbox_stack_tree *tree = cbox_profiler_tree();
	const cbox_arena *arena = cbox_profiler_arena();

	zval frame_list, top_list, stack_list;
	cbox_function_total *totals;
	uint32_t index, emitted = 0;

	array_init(profile);

	add_assoc_long(profile, "sample_count", (zend_long) cbox_profiler_sample_count());
	add_assoc_long(profile, "period_ns", (zend_long) cbox_profiler_period_ns());
	/* Honest about what was actually measured — the fallback backend is wall clock. */
	add_assoc_string(profile, "clock", cbox_timer_is_cpu_time() ? "cpu" : "wall");
	add_assoc_long(profile, "dropped", (zend_long) cbox_profiler_dropped());

	/*
	 * Carried on the profile itself, not just in counters: anyone rendering
	 * this needs to know how much of it was booked late without having to
	 * correlate two structures.
	 */
	add_assoc_long(profile, "deferred_samples", (zend_long) cbox_profiler_deferred_samples());
	add_assoc_long(profile, "deferred_events", (zend_long) cbox_profiler_deferred_events());
	add_assoc_long(profile, "max_deferred", (zend_long) cbox_profiler_max_deferred());
	add_assoc_long(profile, "timer_overruns", (zend_long) cbox_profiler_timer_overruns());

	/* Frames are emitted once and referenced by id — never repeated per sample. */
	array_init_size(&frame_list, frames->count);

	for (index = 0; index < frames->count; index++) {
		const cbox_frame *frame = &frames->frames[index];
		const char *name = cbox_arena_at(arena, frame->name_off);
		const char *file = cbox_arena_at(arena, frame->file_off);
		zval entry;

		array_init_size(&entry, 3);
		add_assoc_stringl(&entry, "function", name != NULL ? name : "", frame->name_len);

		if (file != NULL) {
			add_assoc_stringl(&entry, "file", file, frame->file_len);
		} else {
			add_assoc_null(&entry, "file");
		}

		add_assoc_long(&entry, "line", (zend_long) frame->line);
		add_next_index_zval(&frame_list, &entry);
	}

	add_assoc_zval(profile, "frames", &frame_list);

	/* Self samples per function, folded back out of the call tree. */
	array_init(&top_list);
	totals = frames->count > 0
		? (cbox_function_total *) ecalloc(frames->count, sizeof(cbox_function_total))
		: NULL;

	if (totals != NULL) {
		for (index = 0; index < frames->count; index++) {
			totals[index].frame_id = index;
		}

		for (index = 1; index < tree->count; index++) { /* node 0 is the root sentinel */
			const cbox_node *node = &tree->nodes[index];

			if (node->frame_id < frames->count) {
				totals[node->frame_id].samples += node->samples;
			}
		}

		qsort(totals, frames->count, sizeof(cbox_function_total), cbox_function_total_cmp);

		for (index = 0; index < frames->count && emitted < CBOX_TOP_FUNCTIONS_MAX; index++) {
			zval entry;

			if (totals[index].samples == 0) {
				break;
			}

			array_init_size(&entry, 2);
			add_assoc_long(&entry, "frame_id", (zend_long) totals[index].frame_id);
			add_assoc_long(&entry, "samples", (zend_long) totals[index].samples);
			add_next_index_zval(&top_list, &entry);
			emitted++;
		}

		efree(totals);
	}

	add_assoc_zval(profile, "top_functions", &top_list);

	if (!include_stacks) {
		add_assoc_null(profile, "stacks");
		return;
	}

	/*
	 * Flat triples (parent_node, frame_id, samples); a node's own id is its
	 * position + 1, because node 0 is the root and is never emitted.
	 */
	array_init_size(&stack_list, tree->count > 0 ? tree->count - 1 : 0);

	for (index = 1; index < tree->count; index++) {
		const cbox_node *node = &tree->nodes[index];
		zval entry;

		array_init_size(&entry, 3);
		add_next_index_long(&entry, (zend_long) node->parent);
		add_next_index_long(&entry, (zend_long) node->frame_id);
		add_next_index_long(&entry, (zend_long) node->samples);
		add_next_index_zval(&stack_list, &entry);
	}

	add_assoc_zval(profile, "stacks", &stack_list);
}

/*
 * A guess, and only a guess — whoever calls begin() knows better and can say
 * so. It exists so an automatic unit is not labelled "other" on the overwhelming
 * majority of installs, which are FPM serving HTTP.
 */
static cbox_unit_type cbox_unit_type_for_sapi(void)
{
	const char *name = sapi_module.name;

	if (name == NULL) {
		return CBOX_UNIT_OTHER;
	}

	if (strcmp(name, "cli") == 0 || strcmp(name, "phpdbg") == 0) {
		return CBOX_UNIT_COMMAND;
	}

	return CBOX_UNIT_HTTP;
}

/*
 * Detect that we are running in a forked child and rebuild whatever did not
 * survive.
 *
 * pcntl_fork() copies this extension's memory wholesale, including the flags
 * saying a POSIX timer exists and a sampler thread is running. Neither is
 * true in the child: POSIX says per-thread timers are not inherited, and
 * threads are not either. Without this, a forked worker reports profiling as
 * active and collects nothing, forever.
 *
 * A pid check rather than pthread_atfork: it needs no registration, cannot be
 * bypassed by a fork we did not see, and the only places that care are the
 * ones that call it.
 */
static void cbox_detect_fork(void)
{
	pid_t current = getpid();

	if (CBOX_G(owner_pid) == current) {
		return;
	}

	CBOX_G(owner_pid) = current;

	if (!CBOX_G(active)) {
		return;
	}

	cbox_profiler_after_fork();
	cbox_timer_after_fork();

	/* The inherited unit belongs to the parent's work, not ours. */
	cbox_unit_reset(&CBOX_G(unit));
	cbox_ops_reset(&CBOX_G(ops));

	/* And the inherited sink descriptor still points at the parent's file. */
	if (CBOX_G(crash_enabled)) {
		cbox_crash_open_sink();
	}
}

/* ----------------------------------------------------------- unit lifecycle */

/* Trace ids are optional and independent of how the unit was opened. */
static void cbox_unit_apply_trace(cbox_unit_state *unit, HashTable *context)
{
	zval *value;

	if (context == NULL) {
		return;
	}

	if ((value = zend_hash_str_find(context, "trace_id", sizeof("trace_id") - 1)) != NULL
		&& Z_TYPE_P(value) == IS_STRING
		&& cbox_hex_decode(Z_STRVAL_P(value), Z_STRLEN_P(value), unit->trace_id, CBOX_TRACE_ID_BYTES)
		&& !cbox_bytes_are_zero(unit->trace_id, CBOX_TRACE_ID_BYTES)
	) {
		unit->has_trace = true;
	}

	if ((value = zend_hash_str_find(context, "span_id", sizeof("span_id") - 1)) != NULL
		&& Z_TYPE_P(value) == IS_STRING) {
		cbox_hex_decode(Z_STRVAL_P(value), Z_STRLEN_P(value), unit->span_id, CBOX_SPAN_ID_BYTES);
	}
}

/*
 * Opens a unit. Shared by the automatic RINIT path and by begin(), so the two
 * cannot drift — an automatic unit is an ordinary unit that nobody has claimed
 * yet.
 */
static uint32_t cbox_unit_open(
	cbox_unit_type type,
	bool           sampled,
	bool           want_profile,
	zend_long      period_us,
	zend_long      max_depth,
	bool           automatic
) {
	cbox_unit_state *unit = &CBOX_G(unit);
	const char *label;

	period_us = cbox_clamp(period_us, CBOX_PERIOD_US_MIN, CBOX_PERIOD_US_MAX);
	max_depth = cbox_clamp(max_depth, 1, CBOX_DEPTH_MAX);

	cbox_unit_reset(unit);
	cbox_ops_reset(&CBOX_G(ops));

	if (++CBOX_G(next_handle) == 0) {
		CBOX_G(next_handle) = 1;
	}

	unit->handle = CBOX_G(next_handle);
	unit->type = (uint8_t) type;
	unit->sampled = sampled;
	unit->automatic = automatic;
	unit->adopted = false;
	unit->start_ns = cbox_now_ns();

	cbox_gc_counters(&CBOX_G(gc_runs_at_begin), &CBOX_G(gc_collected_at_begin));

	label = cbox_unit_type_name(type);
	cbox_crumbs_push(&CBOX_G(crumbs), CBOX_CRUMB_UNIT_BEGIN, (uint8_t) type,
		label, strlen(label), unit->start_ns);

	if (want_profile && sampled && CBOX_G(profiler_enabled)) {
		/*
		 * Only an automatic unit gets a deadline. An explicit one has an owner
		 * who is going to call finish(); an automatic one might be sitting in a
		 * queue worker that never will.
		 */
		uint64_t limit = automatic
			? (uint64_t) CBOX_G(auto_max_ms) * 1000000ull
			: 0;

		if (cbox_profiler_start((uint64_t) period_us * 1000ull, (uint32_t) max_depth, limit) == 0) {
			unit->profiling = true;
		}
	} else {
		/* Still clear the native state so the last unit cannot bleed into this one. */
		cbox_profiler_reset();
	}

	return unit->handle;
}

/* ------------------------------------------------------------------ functions */

PHP_FUNCTION(cbox_telemetry_version)
{
	ZEND_PARSE_PARAMETERS_NONE();

	RETURN_STRING(PHP_CBOX_TELEMETRY_VERSION);
}

PHP_FUNCTION(cbox_telemetry_status)
{
	zval limits, hooks;

	ZEND_PARSE_PARAMETERS_NONE();

	array_init(return_value);

	add_assoc_string(return_value, "version", PHP_CBOX_TELEMETRY_VERSION);
	add_assoc_long(return_value, "php_api", PHP_API_VERSION);
	add_assoc_bool(return_value, "enabled", CBOX_G(enabled) && CBOX_G(active));
	add_assoc_bool(return_value, "thread_safe",
#ifdef ZTS
		1
#else
		0
#endif
	);
	add_assoc_bool(return_value, "debug",
#if ZEND_DEBUG
		1
#else
		0
#endif
	);

	add_assoc_bool(return_value, "profiler_enabled", CBOX_G(profiler_enabled));
	add_assoc_bool(return_value, "profiler_running", cbox_profiler_running());
	add_assoc_string(return_value, "timer_backend", cbox_timer_backend());
	add_assoc_bool(return_value, "timer_cpu_time", cbox_timer_is_cpu_time());
	add_assoc_long(return_value, "timer_signal", (zend_long) cbox_timer_signal_number());

	add_assoc_string(return_value, "hooks", cbox_hooks_active());
	add_assoc_bool(return_value, "hooks_armed", CBOX_G(hooks_armed));
	add_assoc_string(return_value, "profiler_status",
		CBOX_G(profiler_reason) != NULL ? CBOX_G(profiler_reason) : "unknown");

	add_assoc_string(return_value, "crash_recorder", cbox_crash_state_name(cbox_crash_state()));

	if (cbox_crash_sink_path() != NULL) {
		add_assoc_string(return_value, "crash_path", cbox_crash_sink_path());
	} else {
		add_assoc_null(return_value, "crash_path");
	}

	add_assoc_bool(return_value, "auto", CBOX_G(auto_start));
	add_assoc_long(return_value, "unit_handle", (zend_long) CBOX_G(unit).handle);
	add_assoc_bool(return_value, "unit_automatic", CBOX_G(unit).automatic);
	add_assoc_long(return_value, "profiler_samples", (zend_long) cbox_profiler_sample_count());
	add_assoc_long(return_value, "profiler_dropped", (zend_long) cbox_profiler_dropped());
	add_assoc_long(return_value, "arena_peak_bytes", (zend_long) cbox_profiler_arena_peak());
	add_assoc_long(return_value, "breadcrumbs_written", (zend_long) CBOX_G(crumbs).written);

	array_init(&limits);
	add_assoc_long(&limits, "period_us", CBOX_G(period_us));
	add_assoc_long(&limits, "max_depth", CBOX_G(max_depth));
	add_assoc_long(&limits, "max_frames", CBOX_G(max_frames));
	add_assoc_long(&limits, "max_nodes", CBOX_G(max_nodes));
	add_assoc_long(&limits, "breadcrumbs", (zend_long) CBOX_G(crumbs).capacity);
	add_assoc_long(&limits, "auto_max_ms", CBOX_G(auto_max_ms));
	add_assoc_zval(return_value, "limits", &limits);

	/*
	 * Requested is configuration; installed is reality. They differ whenever an
	 * extension is not present — "redis enabled, nothing hooked" is the answer
	 * to "why are there no redis.connect timings", and it is not one the INI
	 * alone can give.
	 */
	{
		uint32_t group_count = 0;
		uint32_t index;
		const cbox_hook_group *groups = cbox_hooks_groups(&group_count);
		zval installed;

		array_init(&hooks);

		for (index = 0; index < group_count; index++) {
			zval entry;

			array_init_size(&entry, 4);
			add_assoc_bool(&entry, "requested", groups[index].requested);
			add_assoc_long(&entry, "installed", (zend_long) groups[index].installed);
			add_assoc_long(&entry, "unavailable", (zend_long) groups[index].missing);
			add_assoc_bool(&entry, "active", groups[index].installed > 0);
			add_assoc_zval(&hooks, groups[index].label, &entry);
		}

		add_assoc_zval(return_value, "hook_detail", &hooks);

		array_init(&installed);

		for (index = 0; index < cbox_hooks_installed_count(); index++) {
			const char *name = cbox_hooks_installed_name(index);

			if (name != NULL) {
				add_next_index_string(&installed, name);
			}
		}

		add_assoc_zval(return_value, "hooks_installed", &installed);
	}
}

PHP_FUNCTION(cbox_telemetry_begin)
{
	HashTable *context = NULL;
	cbox_unit_state *unit = &CBOX_G(unit);
	zval *value;
	zend_long period_us = CBOX_G(period_us);
	zend_long max_depth = CBOX_G(max_depth);
	bool want_profile = true;
	bool sampled = true;
	bool explicit_type = false;
	cbox_unit_type type = CBOX_UNIT_OTHER;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_ARRAY_HT_OR_NULL(context)
	ZEND_PARSE_PARAMETERS_END();

	if (!CBOX_G(active) || !CBOX_G(enabled)) {
		RETURN_LONG(0);
	}

	cbox_detect_fork();

	if (context != NULL) {
		if ((value = zend_hash_str_find(context, "unit", sizeof("unit") - 1)) != NULL
			&& Z_TYPE_P(value) == IS_STRING) {
			type = cbox_unit_type_from(Z_STRVAL_P(value), Z_STRLEN_P(value));
			explicit_type = true;
		}

		if ((value = zend_hash_str_find(context, "sampled", sizeof("sampled") - 1)) != NULL) {
			sampled = zend_is_true(value);
		}

		if ((value = zend_hash_str_find(context, "profile", sizeof("profile") - 1)) != NULL) {
			want_profile = zend_is_true(value);
		}

		if ((value = zend_hash_str_find(context, "period_us", sizeof("period_us") - 1)) != NULL
			&& Z_TYPE_P(value) == IS_LONG) {
			period_us = Z_LVAL_P(value);
		}

		if ((value = zend_hash_str_find(context, "max_depth", sizeof("max_depth") - 1)) != NULL
			&& Z_TYPE_P(value) == IS_LONG) {
			max_depth = Z_LVAL_P(value);
		}
	}

	/*
	 * Adopt an automatic unit rather than restarting it. The samples taken
	 * before this call are the framework booting — autoloading, providers,
	 * config — which is exactly the part a middleware-level begin() could
	 * never see. Throwing them away to start a "clean" unit would discard the
	 * most interesting half of a slow cold request.
	 */
	if (unit->handle != 0 && unit->automatic && !unit->adopted) {
		unit->adopted = true;
		unit->sampled = sampled;

		if (explicit_type) {
			unit->type = (uint8_t) type;
		}

		if (!sampled || !want_profile) {
			/* The caller says this one is not worth keeping. */
			cbox_profiler_stop();
			cbox_profiler_reset();
			unit->profiling = false;
		} else if (!unit->profiling && CBOX_G(profiler_enabled)) {
			/* Automatic start did not profile, but this caller wants one. */
			if (cbox_profiler_start(
					(uint64_t) cbox_clamp(period_us, CBOX_PERIOD_US_MIN, CBOX_PERIOD_US_MAX) * 1000ull,
					(uint32_t) cbox_clamp(max_depth, 1, CBOX_DEPTH_MAX),
					0) == 0) {
				unit->profiling = true;
			}
		}
		/*
		 * A profile already running keeps its period. Re-arming at a new one
		 * would mean discarding the samples we adopted this unit for.
		 */

		cbox_unit_apply_trace(unit, context);

		RETURN_LONG((zend_long) unit->handle);
	}

	/*
	 * Units do not nest. A second begin() abandons the first rather than
	 * stacking, so a caller that leaks a handle degrades to "last one wins"
	 * instead of leaving a timer armed forever.
	 */
	if (unit->handle != 0) {
		cbox_profiler_stop();
	}

	cbox_unit_open(type, sampled, want_profile, period_us, max_depth, false);
	cbox_unit_apply_trace(unit, context);

	RETURN_LONG((zend_long) unit->handle);
}

PHP_FUNCTION(cbox_telemetry_finish)
{
	zend_long handle;
	bool include_profile = false;
	bool include_stacks = false;
	cbox_unit_state *unit = &CBOX_G(unit);
	cbox_op_state *ops = &CBOX_G(ops);
	zval operations, counters, profile;
	uint64_t now, duration, gc_runs, gc_collected;
	int index;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_LONG(handle)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL(include_profile)
		Z_PARAM_BOOL(include_stacks)
	ZEND_PARSE_PARAMETERS_END();

	/*
	 * Handle 0 means "whichever unit is open". That is what makes automatic
	 * instrumentation usable from a terminate hook: the caller never saw a
	 * begin() and has no handle to quote back at us.
	 *
	 * Any other unknown handle is not an error either — the caller simply gets
	 * nothing back.
	 */
	if (!CBOX_G(active) || unit->handle == 0) {
		RETURN_EMPTY_ARRAY();
	}

	if (handle != 0 && (zend_long) unit->handle != handle) {
		RETURN_EMPTY_ARRAY();
	}

	now = cbox_now_ns();
	duration = now > unit->start_ns ? now - unit->start_ns : 0;

	cbox_profiler_stop();
	cbox_gc_counters(&gc_runs, &gc_collected);

	array_init(return_value);
	add_assoc_long(return_value, "duration_ns", (zend_long) duration);
	add_assoc_string(return_value, "unit", cbox_unit_type_name((cbox_unit_type) unit->type));
	add_assoc_bool(return_value, "sampled", unit->sampled);
	add_assoc_bool(return_value, "profiling", unit->profiling);
	add_assoc_bool(return_value, "automatic", unit->automatic);

	array_init(&operations);

	for (index = CBOX_OP_NONE + 1; index < CBOX_OP_MAX; index++) {
		const cbox_op_agg *agg = &ops->agg[index];
		zval entry;

		if (agg->count == 0) {
			continue;
		}

		array_init_size(&entry, 3);
		add_assoc_long(&entry, "count", (zend_long) agg->count);
		add_assoc_long(&entry, "total_ns", (zend_long) agg->total_ns);
		add_assoc_long(&entry, "max_ns", (zend_long) agg->max_ns);
		add_assoc_zval(&operations, cbox_op_name((cbox_op_type) index), &entry);
	}

	add_assoc_zval(return_value, "operations", &operations);

	array_init(&counters);
	add_assoc_long(&counters, "profiler.samples", (zend_long) cbox_profiler_sample_count());
	add_assoc_long(&counters, "profiler.dropped", (zend_long) cbox_profiler_dropped());
	add_assoc_long(&counters, "profiler.deferred_samples",
		(zend_long) cbox_profiler_deferred_samples());
	add_assoc_long(&counters, "profiler.deferred_events",
		(zend_long) cbox_profiler_deferred_events());
	add_assoc_long(&counters, "profiler.max_deferred", (zend_long) cbox_profiler_max_deferred());
	add_assoc_long(&counters, "profiler.timer_overruns",
		(zend_long) cbox_profiler_timer_overruns());
	add_assoc_long(&counters, "profiler.period_ns", (zend_long) cbox_profiler_period_ns());
	add_assoc_long(&counters, "gc.runs",
		(zend_long) (gc_runs - CBOX_G(gc_runs_at_begin)));
	add_assoc_long(&counters, "gc.collected",
		(zend_long) (gc_collected - CBOX_G(gc_collected_at_begin)));
	add_assoc_long(&counters, "ops.overflow", (zend_long) ops->overflow);
	add_assoc_long(&counters, "breadcrumbs.written", (zend_long) CBOX_G(crumbs).written);
	add_assoc_long(&counters, "arena.peak_bytes", (zend_long) cbox_profiler_arena_peak());
	add_assoc_long(&counters, "profiler.frame_capacity_hits",
		(zend_long) cbox_profiler_frame_capacity_hits());
	add_assoc_long(&counters, "profiler.node_capacity_hits",
		(zend_long) cbox_profiler_node_capacity_hits());
	add_assoc_bool(&counters, "profiler.capped", cbox_profiler_capped());
	add_assoc_long(&counters, "profiler.frames", (zend_long) cbox_profiler_frames()->count);
	add_assoc_long(&counters, "profiler.nodes", (zend_long) cbox_profiler_tree()->count);
	add_assoc_zval(return_value, "counters", &counters);

	if (include_profile && unit->profiling && cbox_profiler_sample_count() > 0) {
		cbox_profile_into(&profile, include_stacks);
		add_assoc_zval(return_value, "profile", &profile);
	} else {
		add_assoc_null(return_value, "profile");
	}

	cbox_crumbs_push(&CBOX_G(crumbs), CBOX_CRUMB_UNIT_END, unit->type,
		cbox_unit_type_name((cbox_unit_type) unit->type),
		strlen(cbox_unit_type_name((cbox_unit_type) unit->type)), now);

	/*
	 * Reset immediately rather than at the next begin(): a worker that runs
	 * thousands of jobs must not be holding the previous job's profile while
	 * it waits for the next one.
	 */
	cbox_profiler_reset();
	cbox_ops_reset(ops);
	cbox_unit_reset(unit);
}

typedef struct _cbox_drain_context {
	zval *records;
} cbox_drain_context;

static bool cbox_drain_visit(const cbox_crash_record *record, void *context)
{
	cbox_drain_context *drain = (cbox_drain_context *) context;
	zval entry, crumbs;
	char hex[CBOX_TRACE_ID_BYTES * 2];
	uint32_t index;

	array_init(&entry);

	add_assoc_long(&entry, "signal", (zend_long) record->signal);
	add_assoc_string(&entry, "signal_name", cbox_signal_name(record->signal));
	add_assoc_long(&entry, "si_code", (zend_long) record->si_code);
	add_assoc_long(&entry, "pid", (zend_long) record->pid);
	add_assoc_long(&entry, "timestamp_ns", (zend_long) record->realtime_ns);
	add_assoc_string(&entry, "unit", cbox_unit_type_name((cbox_unit_type) record->unit_type));
	add_assoc_long(&entry, "unit_duration_ns", (zend_long)
		(record->monotonic_ns > record->unit_start_ns
			? record->monotonic_ns - record->unit_start_ns
			: 0));

	if (record->has_trace) {
		cbox_hex_encode(record->trace_id, CBOX_TRACE_ID_BYTES, hex);
		add_assoc_stringl(&entry, "trace_id", hex, CBOX_TRACE_ID_BYTES * 2);
		cbox_hex_encode(record->span_id, CBOX_SPAN_ID_BYTES, hex);
		add_assoc_stringl(&entry, "span_id", hex, CBOX_SPAN_ID_BYTES * 2);
	} else {
		add_assoc_null(&entry, "trace_id");
		add_assoc_null(&entry, "span_id");
	}

	if (record->current_op != CBOX_OP_NONE && record->current_op < CBOX_OP_MAX) {
		add_assoc_string(&entry, "operation", cbox_op_name((cbox_op_type) record->current_op));
		add_assoc_long(&entry, "operation_elapsed_ns", (zend_long)
			(record->monotonic_ns > record->current_op_start_ns
				? record->monotonic_ns - record->current_op_start_ns
				: 0));
	} else {
		add_assoc_null(&entry, "operation");
		add_assoc_null(&entry, "operation_elapsed_ns");
	}

	array_init_size(&crumbs, record->crumb_count);

	for (index = 0; index < record->crumb_count && index < CBOX_CRASH_CRUMBS; index++) {
		const cbox_crumb *crumb = &record->crumbs[index];
		size_t label_len = crumb->label_len > CBOX_CRUMB_LABEL_MAX
			? CBOX_CRUMB_LABEL_MAX
			: crumb->label_len;
		zval item;

		array_init_size(&item, 4);
		add_assoc_long(&item, "seq", (zend_long) crumb->seq);
		add_assoc_string(&item, "type", cbox_crumb_type_name(crumb->type));
		add_assoc_stringl(&item, "label", crumb->label, label_len);
		add_assoc_long(&item, "ts_ns", (zend_long) crumb->ts_ns);
		add_next_index_zval(&crumbs, &item);
	}

	add_assoc_zval(&entry, "breadcrumbs", &crumbs);
	add_next_index_zval(drain->records, &entry);

	return true;
}

PHP_FUNCTION(cbox_telemetry_drain_crashes)
{
	zend_long max = 32;
	cbox_drain_context drain;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(max)
	ZEND_PARSE_PARAMETERS_END();

	array_init(return_value);

	if (!CBOX_G(active) || CBOX_G(crash_dir) == NULL || max <= 0) {
		return;
	}

	max = cbox_clamp(max, 1, 1024);
	drain.records = return_value;

	cbox_crash_drain(CBOX_G(crash_dir), (uint32_t) max, cbox_drain_visit, &drain);
}

/* ------------------------------------------------------------------------ INI */

PHP_INI_BEGIN()
	STD_PHP_INI_BOOLEAN("cbox_telemetry.enabled", "1", PHP_INI_SYSTEM, OnUpdateBool,
		enabled, zend_cbox_telemetry_globals, cbox_telemetry_globals)
	STD_PHP_INI_BOOLEAN("cbox_telemetry.profiler.enabled", "1", PHP_INI_SYSTEM, OnUpdateBool,
		profiler_enabled, zend_cbox_telemetry_globals, cbox_telemetry_globals)
	STD_PHP_INI_ENTRY("cbox_telemetry.profiler.period_us", "1000", PHP_INI_ALL, OnUpdateLong,
		period_us, zend_cbox_telemetry_globals, cbox_telemetry_globals)
	STD_PHP_INI_ENTRY("cbox_telemetry.profiler.max_depth", "64", PHP_INI_ALL, OnUpdateLong,
		max_depth, zend_cbox_telemetry_globals, cbox_telemetry_globals)
	STD_PHP_INI_ENTRY("cbox_telemetry.profiler.max_frames", "4096", PHP_INI_SYSTEM, OnUpdateLong,
		max_frames, zend_cbox_telemetry_globals, cbox_telemetry_globals)
	STD_PHP_INI_ENTRY("cbox_telemetry.profiler.max_nodes", "16384", PHP_INI_SYSTEM, OnUpdateLong,
		max_nodes, zend_cbox_telemetry_globals, cbox_telemetry_globals)
	STD_PHP_INI_BOOLEAN("cbox_telemetry.hooks.pdo", "1", PHP_INI_SYSTEM, OnUpdateBool,
		hook_pdo, zend_cbox_telemetry_globals, cbox_telemetry_globals)
	STD_PHP_INI_BOOLEAN("cbox_telemetry.hooks.redis", "1", PHP_INI_SYSTEM, OnUpdateBool,
		hook_redis, zend_cbox_telemetry_globals, cbox_telemetry_globals)
	STD_PHP_INI_BOOLEAN("cbox_telemetry.hooks.curl", "1", PHP_INI_SYSTEM, OnUpdateBool,
		hook_curl, zend_cbox_telemetry_globals, cbox_telemetry_globals)
	STD_PHP_INI_BOOLEAN("cbox_telemetry.hooks.streams", "0", PHP_INI_SYSTEM, OnUpdateBool,
		hook_streams, zend_cbox_telemetry_globals, cbox_telemetry_globals)
	STD_PHP_INI_ENTRY("cbox_telemetry.breadcrumbs.size", "256", PHP_INI_SYSTEM, OnUpdateLong,
		crumb_capacity, zend_cbox_telemetry_globals, cbox_telemetry_globals)
	STD_PHP_INI_BOOLEAN("cbox_telemetry.crash.enabled", "1", PHP_INI_SYSTEM, OnUpdateBool,
		crash_enabled, zend_cbox_telemetry_globals, cbox_telemetry_globals)
	STD_PHP_INI_ENTRY("cbox_telemetry.crash.dir", "/tmp/cbox-telemetry", PHP_INI_SYSTEM, OnUpdateString,
		crash_dir, zend_cbox_telemetry_globals, cbox_telemetry_globals)
	STD_PHP_INI_BOOLEAN("cbox_telemetry.auto", "0", PHP_INI_SYSTEM, OnUpdateBool,
		auto_start, zend_cbox_telemetry_globals, cbox_telemetry_globals)
	STD_PHP_INI_ENTRY("cbox_telemetry.auto_max_ms", "60000", PHP_INI_SYSTEM, OnUpdateLong,
		auto_max_ms, zend_cbox_telemetry_globals, cbox_telemetry_globals)
PHP_INI_END()

/* ------------------------------------------------------------------ lifecycle */

static PHP_GINIT_FUNCTION(cbox_telemetry)
{
#if defined(COMPILE_DL_CBOX_TELEMETRY) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	memset(cbox_telemetry_globals, 0, sizeof(*cbox_telemetry_globals));
}

PHP_MINIT_FUNCTION(cbox_telemetry)
{
	size_t arena_bytes;

#if defined(ZTS) && defined(COMPILE_DL_CBOX_TELEMETRY)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif

	REGISTER_INI_ENTRIES();

	if (!CBOX_G(enabled)) {
		return SUCCESS;
	}

	CBOX_G(period_us) = cbox_clamp(CBOX_G(period_us), CBOX_PERIOD_US_MIN, CBOX_PERIOD_US_MAX);
	CBOX_G(max_depth) = cbox_clamp(CBOX_G(max_depth), 1, CBOX_DEPTH_MAX);
	CBOX_G(max_frames) = cbox_clamp(CBOX_G(max_frames), CBOX_FRAMES_MIN, CBOX_FRAMES_MAX);
	CBOX_G(max_nodes) = cbox_clamp(CBOX_G(max_nodes), CBOX_NODES_MIN, CBOX_NODES_MAX);
	CBOX_G(crumb_capacity) = cbox_clamp(CBOX_G(crumb_capacity), CBOX_CRUMBS_MIN, CBOX_CRUMBS_MAX);

	/* Room for one name and one file path per frame, generously rounded. */
	arena_bytes = (size_t) CBOX_G(max_frames) * 256u;

	if (cbox_crumbs_init(&CBOX_G(crumbs), (uint32_t) CBOX_G(crumb_capacity)) != 0) {
		return SUCCESS; /* fail open: no telemetry beats no PHP */
	}

	if (CBOX_G(profiler_enabled)) {
		if (cbox_profiler_init(
				(uint32_t) CBOX_G(max_frames),
				(uint32_t) CBOX_G(max_nodes),
				arena_bytes) != 0
		) {
			CBOX_G(profiler_enabled) = false;
			CBOX_G(profiler_reason) = "unavailable: could not allocate profile storage";
		} else if (cbox_timer_init() != 0) {
			CBOX_G(profiler_enabled) = false;
			CBOX_G(profiler_reason) = "unavailable: timer could not be created";
		} else {
			cbox_profiler_install();
			CBOX_G(profiler_reason) = "ready";
		}
	} else {
		CBOX_G(profiler_reason) = "disabled by configuration";
	}

	CBOX_G(hooks_armed) = CBOX_G(hook_pdo) || CBOX_G(hook_redis)
		|| CBOX_G(hook_curl) || CBOX_G(hook_streams);

	if (CBOX_G(crash_enabled)) {
		cbox_crash_install(
			CBOX_G(crash_dir),
			&CBOX_G(crumbs),
			&CBOX_G(unit),
			&CBOX_G(ops)
		);
	}

	CBOX_G(active) = true;
	CBOX_G(owner_pid) = getpid();

	return SUCCESS;
}

PHP_RINIT_FUNCTION(cbox_telemetry)
{
	/*
	 * The functions we patch belong to other extensions, whose MINIT may run
	 * after ours — PDO and Redis simply do not exist yet at that point. The
	 * first request is the earliest moment the whole function table is real.
	 */
	if (CBOX_G(active) && CBOX_G(hooks_armed)) {
		cbox_hooks_install(
			CBOX_G(hook_pdo),
			CBOX_G(hook_redis),
			CBOX_G(hook_curl),
			CBOX_G(hook_streams)
		);
	}

	/*
	 * Also the first point running as the worker after an FPM fork, so it is
	 * where the sink gets opened under the identity that will write it.
	 */
	cbox_detect_fork();

	if (CBOX_G(active) && CBOX_G(crash_enabled)) {
		cbox_crash_open_sink();
	}

	/*
	 * Automatic instrumentation. Starting here rather than waiting for a
	 * caller is the whole point: by the time framework middleware runs, the
	 * expensive part of a cold request — autoloading, service providers,
	 * config and route caching — has already happened, and a profile that
	 * starts afterwards cannot see any of it.
	 *
	 * The unit type is guessed from the SAPI and can be corrected by the first
	 * begin(), which adopts this unit rather than replacing it.
	 */
	if (CBOX_G(active) && CBOX_G(enabled) && CBOX_G(auto_start) && CBOX_G(unit).handle == 0) {
		cbox_unit_open(
			cbox_unit_type_for_sapi(),
			true,
			true,
			CBOX_G(period_us),
			CBOX_G(max_depth),
			true
		);
	}

	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(cbox_telemetry)
{
	cbox_hooks_uninstall();
	cbox_crash_uninstall();
	cbox_profiler_uninstall();
	cbox_profiler_shutdown();
	cbox_timer_shutdown();

	/* Only once every handler that could run on it is back to its original. */
	cbox_sigstack_release();

	cbox_crumbs_destroy(&CBOX_G(crumbs));

	CBOX_G(active) = false;

	UNREGISTER_INI_ENTRIES();

	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(cbox_telemetry)
{
	/*
	 * A fatal error or an exit() inside a unit would otherwise leave the timer
	 * armed and the profile state alive into the next request on this worker.
	 */
	if (CBOX_G(unit).handle != 0) {
		cbox_profiler_stop();
		cbox_profiler_reset();
		cbox_ops_reset(&CBOX_G(ops));
		cbox_unit_reset(&CBOX_G(unit));
	}

	return SUCCESS;
}

PHP_MINFO_FUNCTION(cbox_telemetry)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "cbox_telemetry", CBOX_G(active) ? "enabled" : "disabled");
	php_info_print_table_row(2, "Version", PHP_CBOX_TELEMETRY_VERSION);
	php_info_print_table_row(2, "Timer backend", cbox_timer_backend());
	{
		char signal_text[16];
		snprintf(signal_text, sizeof(signal_text), "%d", cbox_timer_signal_number());
		php_info_print_table_row(2, "Timer signal", signal_text);
	}
	php_info_print_table_row(2, "CPU-time sampling", cbox_timer_is_cpu_time() ? "yes" : "no");
	php_info_print_table_row(2, "Active hooks", cbox_hooks_active());
	php_info_print_table_row(2, "Crash recorder", cbox_crash_state_name(cbox_crash_state()));
	php_info_print_table_row(2, "Crash sink",
		cbox_crash_sink_path() != NULL ? cbox_crash_sink_path() : "-");
	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}

zend_module_entry cbox_telemetry_module_entry = {
	STANDARD_MODULE_HEADER,
	PHP_CBOX_TELEMETRY_NAME,
	ext_functions,
	PHP_MINIT(cbox_telemetry),
	PHP_MSHUTDOWN(cbox_telemetry),
	PHP_RINIT(cbox_telemetry),
	PHP_RSHUTDOWN(cbox_telemetry),
	PHP_MINFO(cbox_telemetry),
	PHP_CBOX_TELEMETRY_VERSION,
	PHP_MODULE_GLOBALS(cbox_telemetry),
	PHP_GINIT(cbox_telemetry),
	NULL,
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_CBOX_TELEMETRY
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(cbox_telemetry)
#endif
