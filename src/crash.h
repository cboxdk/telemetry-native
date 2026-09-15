/*
 * cbox_telemetry — crash recorder.
 *
 * Catches SIGSEGV/SIGABRT/SIGBUS/SIGILL, writes one fixed-width binary record
 * to a pre-opened descriptor, and then gets out of the way so the process dies
 * exactly the way it would have without us — core dump, FPM child accounting
 * and all.
 *
 * The handler does nothing that is not on POSIX's async-signal-safe list:
 * no allocation, no Zend API, no PHP callbacks, no locks, no stdio. Everything
 * that *can* be prepared ahead of time is prepared in cbox_crash_install().
 *
 * Records are smaller than PIPE_BUF and written with a single O_APPEND write,
 * so many workers can share one sink without interleaving.
 */
#ifndef CBOX_CRASH_H
#define CBOX_CRASH_H

#include "breadcrumbs.h"
#include "ops.h"
#include "unit.h"

#define CBOX_CRASH_MAGIC          0x43425843u /* 'CBXC' */
#define CBOX_CRASH_FORMAT_VERSION 2u
#define CBOX_CRASH_CRUMBS         32u
/*
 * One sink per process, named for its pid. Two reasons, both learned the hard
 * way:
 *
 *  - A single shared file has to be compacted when it is drained, and
 *    truncating a file that other processes are still appending to loses
 *    whatever arrived between the last read and the truncate. The crash
 *    handler cannot take a lock to close that window — locking is not
 *    async-signal-safe — so the window cannot be closed, only avoided.
 *
 *  - Under PHP-FPM the master starts as one user and workers run as another.
 *    A sink created during module startup belongs to the master, and a worker
 *    cannot reopen it to drain. Opening per process, on first request, means
 *    the file belongs to whoever actually writes it.
 */
#define CBOX_CRASH_FILE_PREFIX    "crash-"
#define CBOX_CRASH_FILE_SUFFIX    ".bin"

typedef struct _cbox_crash_record {
	uint32_t magic;
	uint16_t format_version;
	uint16_t record_len;

	uint32_t signal;
	int32_t  si_code;

	uint32_t pid;
	uint32_t crumb_count;

	uint64_t realtime_ns;
	uint64_t monotonic_ns;

	uint8_t  trace_id[CBOX_TRACE_ID_BYTES];
	uint8_t  span_id[CBOX_SPAN_ID_BYTES];

	uint8_t  unit_type;
	uint8_t  current_op;
	uint8_t  has_trace;
	uint8_t  reserved;

	uint64_t unit_start_ns;
	uint64_t current_op_start_ns;

	/*
	 * Where it actually went wrong.
	 *
	 * si_addr is the address the faulting access touched; program_counter is
	 * the instruction that touched it, read from the signal's machine context.
	 * module_base is where this extension is mapped, captured outside signal
	 * context — subtract it from the PC and a non-trivial offset says the fault
	 * was ours, while a wild one says it was somebody else's code.
	 *
	 * All three are diagnostics, not telemetry. They exist to answer "whose
	 * instruction was it" without a core dump.
	 */
	uint64_t fault_address;
	uint64_t program_counter;
	uint64_t module_base;

	cbox_crumb crumbs[CBOX_CRASH_CRUMBS];
} cbox_crash_record;

typedef enum _cbox_crash_status {
	CBOX_CRASH_OFF = 0,        /* disabled by configuration */
	CBOX_CRASH_ARMED,          /* handlers installed, sink open */
	CBOX_CRASH_NO_DIRECTORY,   /* directory missing, unusable or not ours */
	CBOX_CRASH_NO_SINK,        /* directory fine, file could not be opened */
	CBOX_CRASH_NO_HANDLER      /* sink fine, sigaction/sigaltstack refused */
} cbox_crash_status;

/*
 * Prepare the sink and install the handlers. The three pointers are read from
 * signal context and must outlive the install (they are module globals).
 */
cbox_crash_status cbox_crash_install(
	const char             *dir,
	const cbox_crumb_ring  *ring,
	const cbox_unit_state  *unit,
	const cbox_op_state    *ops
);

/*
 * Open this process's sink, creating the directory if needed. Idempotent, and
 * cheap to call every request: it only acts when the pid has changed, which is
 * exactly once per worker after a fork.
 */
void cbox_crash_open_sink(void);

void cbox_crash_uninstall(void);

cbox_crash_status cbox_crash_state(void);
const char       *cbox_crash_state_name(cbox_crash_status status);
const char       *cbox_crash_sink_path(void);

/* Visitor gets each decoded record; returning false stops the drain early. */
typedef bool (*cbox_crash_visit_fn)(const cbox_crash_record *record, void *context);

/*
 * Read up to `max` records from the sink, hand them to `visit`, and remove the
 * ones that were handed over. Returns the number visited, or -1 on error.
 */
int cbox_crash_drain(const char *dir, uint32_t max, cbox_crash_visit_fn visit, void *context);

#endif /* CBOX_CRASH_H */
