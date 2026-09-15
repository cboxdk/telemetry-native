/*
 * cbox_telemetry — deep PHP runtime telemetry for the Cbox stack.
 *
 * Optional companion to cboxdk/laravel-telemetry. This extension observes and
 * measures the PHP runtime; it knows nothing about Laravel, owns no semantics,
 * and never touches the network.
 */
#ifndef PHP_CBOX_TELEMETRY_H
#define PHP_CBOX_TELEMETRY_H

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"

#include "src/breadcrumbs.h"
#include "src/ops.h"
#include "src/unit.h"

#define PHP_CBOX_TELEMETRY_NAME    "cbox_telemetry"
#define PHP_CBOX_TELEMETRY_VERSION "0.1.0-dev"

extern zend_module_entry cbox_telemetry_module_entry;
#define phpext_cbox_telemetry_ptr &cbox_telemetry_module_entry

/* Bounds. Configuration is clamped to these, never trusted. */
#define CBOX_PERIOD_US_MIN     100
#define CBOX_PERIOD_US_MAX     100000
#define CBOX_DEPTH_MAX         256
#define CBOX_FRAMES_MIN        64
#define CBOX_FRAMES_MAX        65536
#define CBOX_NODES_MIN         256
#define CBOX_NODES_MAX         262144
#define CBOX_CRUMBS_MIN        16
#define CBOX_CRUMBS_MAX        4096
#define CBOX_TOP_FUNCTIONS_MAX 256
#define CBOX_AUTO_MAX_MS_MIN   1000
#define CBOX_AUTO_MAX_MS_MAX   3600000

ZEND_BEGIN_MODULE_GLOBALS(cbox_telemetry)
	/* INI */
	bool      enabled;
	bool      profiler_enabled;
	zend_long period_us;
	zend_long max_depth;
	zend_long max_frames;
	zend_long max_nodes;
	bool      hook_pdo;
	bool      hook_redis;
	bool      hook_curl;
	bool      hook_streams;
	zend_long crumb_capacity;
	bool      crash_enabled;
	char     *crash_dir;
	bool      auto_start;
	zend_long auto_max_ms;

	/* Runtime */
	cbox_unit_state unit;
	cbox_op_state   ops;
	cbox_crumb_ring crumbs;

	uint32_t next_handle;
	pid_t    owner_pid;      /* the process this native state belongs to */
	uint64_t gc_runs_at_begin;
	uint64_t gc_collected_at_begin;

	bool active;      /* MINIT succeeded and the extension is usable */
	bool hooks_armed; /* at least one hook group was requested */

	/* Why the profiler is not running, when it is not. */
	const char *profiler_reason;
ZEND_END_MODULE_GLOBALS(cbox_telemetry)

ZEND_EXTERN_MODULE_GLOBALS(cbox_telemetry)

#define CBOX_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(cbox_telemetry, v)

#if defined(ZTS) && defined(COMPILE_DL_CBOX_TELEMETRY)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

/* Shared with src/hooks.c. */
void cbox_telemetry_note_op_begin(cbox_op_type type);
void cbox_telemetry_note_op_end(cbox_op_type type);

#endif /* PHP_CBOX_TELEMETRY_H */
