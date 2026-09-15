#ifndef _GNU_SOURCE
# define _GNU_SOURCE 1
#endif

#include "crash.h"
#include "clock.h"
#include "sigstack.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * A torn record would be unparseable. Records are written with one write() to
 * an O_APPEND descriptor and must stay inside the atomic-write window, so this
 * is a build error rather than a runtime surprise.
 */
typedef char cbox_crash_record_fits[(sizeof(cbox_crash_record) <= 4096) ? 1 : -1];

#define CBOX_CRASH_SIGNALS 4
#define CBOX_CRASH_PATH_MAX 512
/* One drain reads at most this much; a runaway sink is truncated, not looped on. */
#define CBOX_CRASH_DRAIN_MAX_BYTES (4u * 1024u * 1024u)

static const int cbox_crash_signals[CBOX_CRASH_SIGNALS] = {
	SIGSEGV, SIGABRT, SIGBUS, SIGILL
};

/*
 * Signal-context state. File statics rather than module globals on purpose:
 * resolving a TSRM cache from a signal handler is not async-signal-safe. This
 * is one of the two places that would need real work before a ZTS build.
 */
static volatile sig_atomic_t cbox_crash_in_progress = 0;
static int                   cbox_crash_fd = -1;
static cbox_crash_record     cbox_crash_record_buffer;
static const cbox_crumb_ring *cbox_crash_ring = NULL;
static const cbox_unit_state *cbox_crash_unit = NULL;
static const cbox_op_state   *cbox_crash_ops = NULL;

static struct sigaction cbox_crash_previous[CBOX_CRASH_SIGNALS];
static bool             cbox_crash_installed = false;
static cbox_crash_status cbox_crash_current_status = CBOX_CRASH_OFF;
static char             cbox_crash_path[CBOX_CRASH_PATH_MAX];

/* ------------------------------------------------------------------ handler */

static void cbox_crash_write_all(int fd, const char *bytes, size_t len)
{
	while (len > 0) {
		ssize_t written = write(fd, bytes, len);

		if (written > 0) {
			bytes += written;
			len -= (size_t) written;
			continue;
		}

		if (written < 0 && errno == EINTR) {
			continue;
		}

		return; /* the process is dying; a failed write is not worth a retry loop */
	}
}

static void cbox_crash_handler(int sig, siginfo_t *info, void *context)
{
	cbox_crash_record *record = &cbox_crash_record_buffer;
	int index;

	/* A crash inside the crash handler must not loop. */
	if (cbox_crash_in_progress) {
		_exit(128 + sig);
	}

	cbox_crash_in_progress = 1;

	if (cbox_crash_fd >= 0) {
		record->signal = (uint32_t) sig;
		record->si_code = info != NULL ? (int32_t) info->si_code : 0;
		record->pid = (uint32_t) getpid();
		record->realtime_ns = cbox_realtime_ns();
		record->monotonic_ns = cbox_now_ns();

		if (cbox_crash_unit != NULL) {
			record->unit_type = cbox_crash_unit->type;
			record->has_trace = cbox_crash_unit->has_trace ? 1 : 0;
			record->unit_start_ns = cbox_crash_unit->start_ns;
			memcpy(record->trace_id, cbox_crash_unit->trace_id, CBOX_TRACE_ID_BYTES);
			memcpy(record->span_id, cbox_crash_unit->span_id, CBOX_SPAN_ID_BYTES);
		}

		if (cbox_crash_ops != NULL) {
			record->current_op = (uint8_t) cbox_ops_current(cbox_crash_ops);
			record->current_op_start_ns = cbox_ops_current_start(cbox_crash_ops);
		}

		record->crumb_count = cbox_crash_ring != NULL
			? cbox_crumbs_snapshot(cbox_crash_ring, record->crumbs, CBOX_CRASH_CRUMBS)
			: 0;

		cbox_crash_write_all(cbox_crash_fd, (const char *) record, sizeof(*record));
	}

	/*
	 * Put the previous disposition back and let the process die the way it
	 * would have without us.
	 *
	 * How that happens depends on where the signal came from. A hardware fault
	 * (si_code > 0: SEGV_MAPERR, BUS_ADRALN, ILL_ILLOPC…) re-traps the moment
	 * we return, so returning is enough and the core dump still points at the
	 * faulting instruction. A signal someone *sent* us (si_code <= 0: SI_USER,
	 * SI_TKILL, and the raise() inside abort()) does not re-trap — returning
	 * there would leave a process that wrote a crash record and then carried
	 * on as if nothing had happened.
	 */
	for (index = 0; index < CBOX_CRASH_SIGNALS; index++) {
		bool delivered_by_hardware;

		if (cbox_crash_signals[index] != sig) {
			continue;
		}

		sigaction(sig, &cbox_crash_previous[index], NULL);

		if (cbox_crash_previous[index].sa_flags & SA_SIGINFO) {
			if (cbox_crash_previous[index].sa_sigaction != NULL) {
				cbox_crash_previous[index].sa_sigaction(sig, info, context);
				return;
			}
		} else if (cbox_crash_previous[index].sa_handler != SIG_DFL
			&& cbox_crash_previous[index].sa_handler != SIG_IGN) {
			cbox_crash_previous[index].sa_handler(sig);
			return;
		}

		delivered_by_hardware = info != NULL && info->si_code > 0;

		if (!delivered_by_hardware) {
			raise(sig);
		}

		return;
	}

	_exit(128 + sig);
}

/* ------------------------------------------------------------------- install */

static bool cbox_crash_prepare_directory(const char *dir)
{
	struct stat info;

	if (dir == NULL || dir[0] == '\0') {
		return false;
	}

	if (mkdir(dir, 0700) == 0) {
		return true;
	}

	if (errno != EEXIST) {
		return false;
	}

	/*
	 * It already exists. Only use it when it is a real directory that we own —
	 * a shared /tmp path is a symlink and ownership trap otherwise.
	 */
	if (lstat(dir, &info) != 0) {
		return false;
	}

	if (!S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode)) {
		return false;
	}

	if (info.st_uid != geteuid()) {
		return false;
	}

	return access(dir, W_OK | X_OK) == 0;
}

static int cbox_crash_open_sink(const char *dir, int flags)
{
	char path[CBOX_CRASH_PATH_MAX];
	int written = snprintf(path, sizeof(path), "%s/%s", dir, CBOX_CRASH_FILENAME);

	if (written <= 0 || (size_t) written >= sizeof(path)) {
		return -1;
	}

	return open(path, flags | O_CLOEXEC | O_NOFOLLOW, 0600);
}

cbox_crash_status cbox_crash_install(
	const char             *dir,
	const cbox_crumb_ring  *ring,
	const cbox_unit_state  *unit,
	const cbox_op_state    *ops
) {
	struct sigaction action;
	int index, written;
	int flags = SA_SIGINFO | SA_RESTART;

	if (cbox_crash_installed) {
		return cbox_crash_current_status;
	}

	if (!cbox_crash_prepare_directory(dir)) {
		cbox_crash_current_status = CBOX_CRASH_NO_DIRECTORY;
		return cbox_crash_current_status;
	}

	cbox_crash_fd = cbox_crash_open_sink(dir, O_WRONLY | O_CREAT | O_APPEND);

	if (cbox_crash_fd < 0) {
		cbox_crash_current_status = CBOX_CRASH_NO_SINK;
		return cbox_crash_current_status;
	}

	written = snprintf(cbox_crash_path, sizeof(cbox_crash_path), "%s/%s", dir, CBOX_CRASH_FILENAME);

	if (written <= 0 || (size_t) written >= sizeof(cbox_crash_path)) {
		cbox_crash_path[0] = '\0';
	}

	memset(&cbox_crash_record_buffer, 0, sizeof(cbox_crash_record_buffer));
	cbox_crash_record_buffer.magic = CBOX_CRASH_MAGIC;
	cbox_crash_record_buffer.format_version = CBOX_CRASH_FORMAT_VERSION;
	cbox_crash_record_buffer.record_len = (uint16_t) sizeof(cbox_crash_record);

	cbox_crash_ring = ring;
	cbox_crash_unit = unit;
	cbox_crash_ops = ops;

	/*
	 * A stack-overflow SIGSEGV can only be caught on an alternate stack. If we
	 * cannot get one, still install — we just lose that particular case.
	 */
	if (cbox_sigstack_ensure()) {
		flags |= SA_ONSTACK;
	}

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = cbox_crash_handler;
	action.sa_flags = flags;
	sigemptyset(&action.sa_mask);

	for (index = 0; index < CBOX_CRASH_SIGNALS; index++) {
		if (sigaction(cbox_crash_signals[index], &action, &cbox_crash_previous[index]) != 0) {
			/* Roll back whatever we managed to install. */
			while (--index >= 0) {
				sigaction(cbox_crash_signals[index], &cbox_crash_previous[index], NULL);
			}

			close(cbox_crash_fd);
			cbox_crash_fd = -1;
			cbox_crash_current_status = CBOX_CRASH_NO_HANDLER;

			return cbox_crash_current_status;
		}
	}

	cbox_crash_installed = true;
	cbox_crash_current_status = CBOX_CRASH_ARMED;

	return cbox_crash_current_status;
}

void cbox_crash_uninstall(void)
{
	int index;

	if (cbox_crash_installed) {
		for (index = 0; index < CBOX_CRASH_SIGNALS; index++) {
			sigaction(cbox_crash_signals[index], &cbox_crash_previous[index], NULL);
		}

		cbox_crash_installed = false;
	}

	if (cbox_crash_fd >= 0) {
		close(cbox_crash_fd);
		cbox_crash_fd = -1;
	}

	cbox_crash_ring = NULL;
	cbox_crash_unit = NULL;
	cbox_crash_ops = NULL;
	cbox_crash_current_status = CBOX_CRASH_OFF;
	cbox_crash_path[0] = '\0';
}

cbox_crash_status cbox_crash_state(void)
{
	return cbox_crash_current_status;
}

const char *cbox_crash_state_name(cbox_crash_status status)
{
	switch (status) {
		case CBOX_CRASH_ARMED:        return "armed";
		case CBOX_CRASH_NO_DIRECTORY: return "unavailable: directory";
		case CBOX_CRASH_NO_SINK:      return "unavailable: sink";
		case CBOX_CRASH_NO_HANDLER:   return "unavailable: handler";
		case CBOX_CRASH_OFF:
		default:                      return "off";
	}
}

const char *cbox_crash_sink_path(void)
{
	return cbox_crash_path[0] == '\0' ? NULL : cbox_crash_path;
}

/* --------------------------------------------------------------------- drain */

static bool cbox_crash_record_valid(const cbox_crash_record *record)
{
	return record->magic == CBOX_CRASH_MAGIC
		&& record->format_version == CBOX_CRASH_FORMAT_VERSION
		&& record->record_len == (uint16_t) sizeof(cbox_crash_record)
		&& record->crumb_count <= CBOX_CRASH_CRUMBS;
}

int cbox_crash_drain(const char *dir, uint32_t max, cbox_crash_visit_fn visit, void *context)
{
	int fd, visited = 0;
	char *buffer = NULL;
	ssize_t got;
	size_t size, offset = 0, consumed = 0;
	struct stat info;

	if (dir == NULL || visit == NULL || max == 0) {
		return -1;
	}

	fd = cbox_crash_open_sink(dir, O_RDWR);

	if (fd < 0) {
		return errno == ENOENT ? 0 : -1;
	}

	/* Advisory, and deliberately not taken by the handler — see the tail copy below. */
	if (flock(fd, LOCK_EX) != 0) {
		close(fd);
		return -1;
	}

	if (fstat(fd, &info) != 0 || info.st_size <= 0) {
		close(fd);
		return 0;
	}

	size = (size_t) info.st_size;

	if (size > CBOX_CRASH_DRAIN_MAX_BYTES) {
		size = CBOX_CRASH_DRAIN_MAX_BYTES;
	}

	buffer = (char *) malloc(size);

	if (buffer == NULL) {
		close(fd);
		return -1;
	}

	got = pread(fd, buffer, size, 0);

	if (got <= 0) {
		free(buffer);
		close(fd);
		return got == 0 ? 0 : -1;
	}

	size = (size_t) got;

	while (offset + sizeof(cbox_crash_record) <= size && (uint32_t) visited < max) {
		const cbox_crash_record *record = (const cbox_crash_record *) (buffer + offset);

		if (!cbox_crash_record_valid(record)) {
			/* Garbage or a format we don't know: skip a byte and resynchronise. */
			offset++;
			continue;
		}

		visited++;
		offset += sizeof(cbox_crash_record);
		consumed = offset;

		if (!visit(record, context)) {
			break;
		}
	}

	/*
	 * Keep whatever we did not hand over, including anything appended by a
	 * crashing process while we were reading (the handler cannot take the lock,
	 * so this is the only thing standing between a concurrent crash and a lost
	 * record).
	 */
	if (consumed > 0) {
		char tail[8192];
		off_t read_at = (off_t) consumed;
		off_t write_at = 0;

		for (;;) {
			ssize_t chunk = pread(fd, tail, sizeof(tail), read_at);

			if (chunk <= 0) {
				break;
			}

			if (pwrite(fd, tail, (size_t) chunk, write_at) != chunk) {
				break;
			}

			read_at += chunk;
			write_at += chunk;
		}

		if (ftruncate(fd, write_at) != 0) {
			/* Leaving the file as-is only risks re-reporting, never data loss. */
		}
	}

	free(buffer);
	close(fd);

	return visited;
}
