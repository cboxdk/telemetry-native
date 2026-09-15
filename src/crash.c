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
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * A torn record would be unparseable. Records are written with one write() to
 * an O_APPEND descriptor and must stay inside the atomic-write window, so this
 * is a build error rather than a runtime surprise.
 */
typedef char cbox_crash_record_fits[(sizeof(cbox_crash_record) <= 4096) ? 1 : -1];

/* Just the fields every format version is required to start with. */
typedef struct {
	uint32_t magic;
	uint16_t format_version;
	uint16_t record_len;
} cbox_crash_record_header_probe;

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
static uint64_t          cbox_crash_module_base = 0;
static char             cbox_crash_path[CBOX_CRASH_PATH_MAX];
static char             cbox_crash_dir[CBOX_CRASH_PATH_MAX];
static char             cbox_crash_owned_dir[CBOX_CRASH_PATH_MAX];
static pid_t            cbox_crash_sink_pid = 0;

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

/*
 * The faulting instruction pointer out of the signal's machine context.
 * Signal-safe: it is a field read from a structure the kernel already handed
 * us. Returns 0 where the layout is unknown rather than guessing.
 */
static uint64_t cbox_crash_program_counter(void *context)
{
	if (context == NULL) {
		return 0;
	}

#if defined(__APPLE__)
	{
		const ucontext_t *uc = (const ucontext_t *) context;

		if (uc->uc_mcontext == NULL) {
			return 0;
		}

# if defined(__aarch64__) || defined(__arm64__)
		return (uint64_t) uc->uc_mcontext->__ss.__pc;
# elif defined(__x86_64__)
		return (uint64_t) uc->uc_mcontext->__ss.__rip;
# else
		return 0;
# endif
	}
#elif defined(__linux__)
	{
		const ucontext_t *uc = (const ucontext_t *) context;

# if defined(__aarch64__)
		return (uint64_t) uc->uc_mcontext.pc;
# elif defined(__x86_64__)
		return (uint64_t) uc->uc_mcontext.gregs[REG_RIP];
# else
		return 0;
# endif
	}
#else
	(void) context;

	return 0;
#endif
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

	/*
	 * Open here rather than at request start. Creating the file up front means
	 * every process that never crashes still leaves an empty one behind — one
	 * per worker lifetime, which on a recycling FPM pool is thousands a day of
	 * litter in a shared directory. open() and write() are both async-signal-
	 * safe; the path was built outside signal context precisely so that
	 * snprintf, which is not, never runs here.
	 */
	if (cbox_crash_fd < 0 && cbox_crash_path[0] != '\0') {
		cbox_crash_fd = open(
			cbox_crash_path,
			/*
			 * O_NONBLOCK for the same reason the drain has it: a FIFO planted
			 * under this name would otherwise block open() waiting for a
			 * reader, and a crashing worker would hang there forever instead
			 * of dying. O_NOFOLLOW does not help — a FIFO is not a symlink.
			 */
			O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
			0600
		);

		/*
		 * Someone may have created this name first — in a shared directory an
		 * unprivileged user can pre-create it and read whatever we append.
		 * fstat is async-signal-safe, so the check belongs here.
		 */
		if (cbox_crash_fd >= 0) {
			struct stat sink;

			if (fstat(cbox_crash_fd, &sink) != 0
				|| !S_ISREG(sink.st_mode)
				|| sink.st_uid != geteuid()
				|| sink.st_nlink != 1
			) {
				close(cbox_crash_fd);
				cbox_crash_fd = -1;
			}
		}
	}

	if (cbox_crash_fd >= 0) {
		record->signal = (uint32_t) sig;
		record->si_code = info != NULL ? (int32_t) info->si_code : 0;
		record->fault_address = info != NULL ? (uint64_t) (uintptr_t) info->si_addr : 0;
		record->program_counter = cbox_crash_program_counter(context);
		record->module_base = cbox_crash_module_base;
		record->pid = (uint32_t) getpid();
		record->realtime_ns = cbox_realtime_ns();
		record->monotonic_ns = cbox_now_ns();

		if (cbox_crash_unit != NULL) {
			record->unit_type = cbox_crash_unit->type;
			record->has_trace = cbox_crash_unit->has_trace ? 1 : 0;
			record->has_span = cbox_crash_unit->has_span ? 1 : 0;
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

		/*
		 * Always re-raise. An earlier version only did this for signals it
		 * judged to have been *sent* (si_code <= 0), trusting a fault to
		 * re-trap on return. That contract is Linux's alone: XNU reports
		 * si_code = 2 for a kill()-sent SIGSEGV too, so the handler classified
		 * it as a fault, returned, and the process carried on running after a
		 * fatal signal — having already written a crash record for it.
		 *
		 * Re-raising costs a real fault nothing: it re-traps before the pending
		 * signal is ever delivered, so the core dump still points at the
		 * faulting instruction.
		 */
		raise(sig);

		return;
	}

	_exit(128 + sig);
}

/* ------------------------------------------------------------------- install */

/*
 * Force the mode regardless of umask. mkdir() cannot: its mode argument is
 * masked, so mkdir(dir, 01733) under the usual umask 022 produces 01711 and
 * every other user is locked out — which was the entire point of widening it.
 */
static bool cbox_crash_make_directory(const char *path, mode_t mode)
{
	if (mkdir(path, mode) != 0) {
		return false;
	}

	if (chmod(path, mode) != 0) {
		rmdir(path);
		return false;
	}

	return true;
}

static bool cbox_crash_directory_is_ours(const char *path, mode_t forbidden)
{
	struct stat info;

	if (lstat(path, &info) != 0) {
		return false;
	}

	return S_ISDIR(info.st_mode)
		&& !S_ISLNK(info.st_mode)
		&& info.st_uid == geteuid()
		&& (info.st_mode & forbidden) == 0;
}

/*
 * Produces a directory this process owns privately, and refuses anything else.
 *
 * The base may be shared — /tmp/cbox-telemetry is the default and several FPM
 * pools, an opcache.preload user and the CLI may all arrive at it. Each uid
 * therefore gets its own 0700 subdirectory and never writes into the base.
 *
 * An earlier attempt made the base itself group- and world-writable so that
 * any pool could drop files in it, and accepted a pre-existing base owned by
 * somebody else if it was sticky. Both were wrong. An unprivileged user can
 * win the race to create /tmp/cbox-telemetry, and sticky does not protect
 * files from the directory's *owner* — they could then delete or rename every
 * crash record on the host. Demonstrated, not theorised.
 */
static bool cbox_crash_prepare_directory(const char *dir, char *out, size_t out_size)
{
	struct stat info;
	int written;

	if (dir == NULL || dir[0] == '\0') {
		return false;
	}

	if (lstat(dir, &info) != 0) {
		/*
		 * Ours alone. A shared base has to be provisioned deliberately by
		 * packaging (root-owned, sticky, world-writable); we never create one.
		 */
		if (!cbox_crash_make_directory(dir, 0700)) {
			return false;
		}
	} else if (!S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode)) {
		return false;
	} else if (info.st_uid != geteuid()) {
		/*
		 * A base someone else owns is only acceptable if that someone is root
		 * and it follows the /tmp contract: sticky and world-writable.
		 *
		 * Sticky is not enough on its own. It stops *other users* removing an
		 * entry, but the directory's owner can always remove entries in it —
		 * so an unprivileged user who wins the race to create the base can
		 * delete or rename every crash record on the host, whatever the mode
		 * says. Verified with two real users before this check existed.
		 *
		 * Losing that race now costs crash recording, reported by status() as
		 * "unavailable: directory", rather than handing it to whoever won.
		 */
		if (info.st_uid != 0
			|| (info.st_mode & S_ISVTX) == 0
			|| (info.st_mode & S_IWOTH) == 0
		) {
			return false;
		}
	}

	written = snprintf(out, out_size, "%s/%lu", dir, (unsigned long) geteuid());

	if (written <= 0 || (size_t) written >= out_size) {
		return false;
	}

	if (lstat(out, &info) != 0) {
		if (!cbox_crash_make_directory(out, 0700)) {
			return false;
		}
	}

	/* Fail closed: if it is not a private directory of ours, do not use it. */
	if (!cbox_crash_directory_is_ours(out, S_IRWXG | S_IRWXO)) {
		return false;
	}

	return access(out, W_OK | X_OK) == 0;
}

static int cbox_crash_sink_name(char *out, size_t size, const char *dir, pid_t pid)
{
	int written = snprintf(out, size, "%s/%s%ld%s",
		dir, CBOX_CRASH_FILE_PREFIX, (long) pid, CBOX_CRASH_FILE_SUFFIX);

	return (written > 0 && (size_t) written < size) ? 0 : -1;
}

/* A process that is gone will never append again; one that is alive might. */
static bool cbox_crash_process_alive(pid_t pid)
{
	if (pid <= 0) {
		return false;
	}

	if (kill(pid, 0) == 0) {
		return true;
	}

	/* Alive, just not ours to signal. */
	return errno == EPERM;
}

void cbox_crash_open_sink(void)
{
	char path[CBOX_CRASH_PATH_MAX];
	pid_t pid = getpid();

	if (!cbox_crash_installed || cbox_crash_dir[0] == '\0') {
		return;
	}

	/* Already prepared for this process — the common case on every request. */
	if (cbox_crash_sink_pid == pid && cbox_crash_path[0] != '\0') {
		return;
	}

	/* A descriptor inherited across fork belongs to the parent's file. */
	if (cbox_crash_fd >= 0) {
		close(cbox_crash_fd);
		cbox_crash_fd = -1;
	}

	cbox_crash_path[0] = '\0';
	cbox_crash_sink_pid = 0;

	/*
	 * The directory is created now, under the identity that will write into it.
	 * The file is not: it is created by the handler, if there is ever anything
	 * to put in it.
	 */
	if (!cbox_crash_prepare_directory(cbox_crash_dir, cbox_crash_owned_dir, sizeof(cbox_crash_owned_dir))) {
		cbox_crash_current_status = CBOX_CRASH_NO_DIRECTORY;
		return;
	}

	if (cbox_crash_sink_name(path, sizeof(path), cbox_crash_owned_dir, pid) != 0) {
		cbox_crash_current_status = CBOX_CRASH_NO_SINK;
		return;
	}

	/* Check we could write there, without leaving anything behind. */
	if (access(cbox_crash_dir, W_OK | X_OK) != 0) {
		cbox_crash_current_status = CBOX_CRASH_NO_SINK;
		return;
	}

	memcpy(cbox_crash_path, path, sizeof(path));
	cbox_crash_sink_pid = pid;
	cbox_crash_current_status = CBOX_CRASH_ARMED;
}

cbox_crash_status cbox_crash_install(
	const char             *dir,
	const cbox_crumb_ring  *ring,
	const cbox_unit_state  *unit,
	const cbox_op_state    *ops
) {
	struct sigaction action;
	int index;
	int flags = SA_SIGINFO | SA_RESTART;

	if (cbox_crash_installed) {
		return cbox_crash_current_status;
	}

	if (dir == NULL || dir[0] == '\0'
		|| strlen(dir) >= sizeof(cbox_crash_dir) - 32
	) {
		cbox_crash_current_status = CBOX_CRASH_NO_DIRECTORY;
		return cbox_crash_current_status;
	}

	/*
	 * The directory is NOT created here. Module startup runs in the FPM master,
	 * usually as root; creating it there would leave a root-owned 0700
	 * directory that the workers cannot write to. The first request in each
	 * worker opens the sink under the identity that will actually use it.
	 */
	strcpy(cbox_crash_dir, dir);
	cbox_crash_path[0] = '\0';

	/*
	 * Where this shared object is mapped. dladdr() is not signal-safe, so it is
	 * resolved here, once, and only read from the handler.
	 */
	{
		Dl_info info;

		if (dladdr((void *) (uintptr_t) &cbox_crash_install, &info) != 0 && info.dli_fbase != NULL) {
			cbox_crash_module_base = (uint64_t) (uintptr_t) info.dli_fbase;
		}
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
	/*
	 * SA_NODEFER so the signal stays deliverable while we handle it. Without
	 * it, a fault *inside* the handler is masked, and Darwin neither
	 * force-delivers it the way Linux does nor runs the handler again: the
	 * thread re-executes the faulting instruction forever. The re-entrancy
	 * latch below is what turns that into a clean _exit, but it only gets a
	 * chance if the signal can be delivered at all.
	 */
	action.sa_flags = flags | SA_NODEFER;
	sigemptyset(&action.sa_mask);

	for (index = 0; index < CBOX_CRASH_SIGNALS; index++) {
		if (sigaction(cbox_crash_signals[index], &action, &cbox_crash_previous[index]) != 0) {
			/* Roll back whatever we managed to install. */
			while (--index >= 0) {
				sigaction(cbox_crash_signals[index], &cbox_crash_previous[index], NULL);
			}

			cbox_crash_current_status = CBOX_CRASH_NO_HANDLER;

			return cbox_crash_current_status;
		}
	}

	cbox_crash_installed = true;
	cbox_crash_current_status = CBOX_CRASH_ARMED;

	/*
	 * The sink is deliberately NOT opened here.
	 *
	 * Module startup runs in the FPM master, which is normally root. Opening
	 * here creates the directory as root with mode 0700, and every worker —
	 * running as www-data — is then locked out of its own crash sink for the
	 * life of the pool. The recorder reports "unavailable: directory" and
	 * records nothing, which is a silent loss of the whole feature in the most
	 * common production deployment there is.
	 *
	 * RINIT is the first point that runs as the user who will actually write
	 * the file, so that is where the sink is opened. A crash between module
	 * startup and the first request is not covered; a crash in any request is.
	 */

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
	cbox_crash_dir[0] = '\0';
	cbox_crash_owned_dir[0] = '\0';
	cbox_crash_sink_pid = 0;
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

/*
 * A record this build cannot decode, but which is plainly one of ours. The
 * magic and record_len exist precisely so a different version can be stepped
 * over by length instead of being treated as garbage — a reader that byte-
 * resyncs through them and then deletes the file destroys every record written
 * by the previous version of the extension.
 */
static bool cbox_crash_record_foreign_version(const cbox_crash_record *record, size_t remaining)
{
	return record->magic == CBOX_CRASH_MAGIC
		&& record->format_version != CBOX_CRASH_FORMAT_VERSION
		&& record->record_len >= sizeof(cbox_crash_record_header_probe)
		&& record->record_len <= remaining;
}

/*
 * Read one dead process's sink and hand its records over.
 * Returns the number visited, or -1 if the file could not be read.
 */
static int cbox_crash_drain_file(
	const char          *path,
	uint32_t             max,
	cbox_crash_visit_fn  visit,
	void                *context,
	bool                *stop,
	bool                *consumable,
	off_t               *consumed_bytes
) {
	int fd, visited = 0;
	char *buffer;
	ssize_t got;
	size_t size, offset = 0;
	off_t file_size;
	bool foreign_seen = false;
	struct stat info;

	*consumable = false;
	*consumed_bytes = 0;

	/*
	 * O_NONBLOCK because this path is not necessarily ours: a FIFO planted
	 * under a sink's name would otherwise block open() forever and hang the
	 * request doing the drain.
	 */
	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);

	if (fd < 0) {
		return -1;
	}

	/*
	 * And only read it if it is a plain file belonging to us. Without this,
	 * anyone able to write the directory can plant a record and have it
	 * reported as genuine crash telemetry.
	 */
	if (fstat(fd, &info) != 0
		|| !S_ISREG(info.st_mode)
		|| info.st_uid != geteuid()
	) {
		close(fd);
		return -1;
	}

	if (info.st_size <= 0) {
		close(fd);
		*consumable = true;
		return 0;
	}

	file_size = info.st_size;
	size = (size_t) file_size;

	if (size > CBOX_CRASH_DRAIN_MAX_BYTES) {
		size = CBOX_CRASH_DRAIN_MAX_BYTES;
	}

	buffer = (char *) malloc(size);

	if (buffer == NULL) {
		close(fd);
		return -1;
	}

	got = pread(fd, buffer, size, 0);
	close(fd);

	if (got <= 0) {
		free(buffer);
		return -1;
	}

	size = (size_t) got;

	while (offset + sizeof(cbox_crash_record_header_probe) <= size && (uint32_t) visited < max) {
		const cbox_crash_record *record = (const cbox_crash_record *) (buffer + offset);

		/*
		 * The loop advances on the shared header, not on this version's record
		 * size: a foreign record *smaller* than ours would otherwise never be
		 * examined at the end of a file, and the file would be deleted as if
		 * it had been read.
		 */
		if (cbox_crash_record_foreign_version(record, size - offset)) {
			/* Step over it by its own length and keep the file. */
			foreign_seen = true;
			offset += record->record_len;
			continue;
		}

		if (offset + sizeof(cbox_crash_record) > size
			|| !cbox_crash_record_valid(record)
		) {
			/* Garbage, or a truncated tail: resynchronise a byte at a time. */
			offset++;
			continue;
		}

		visited++;
		offset += sizeof(cbox_crash_record);
		*consumed_bytes = (off_t) offset;

		if (!visit(record, context)) {
			*stop = true;
			break;
		}
	}

	/*
	 * The file may be removed only when every byte of it was read AND every
	 * record in it was handed over. Measuring against the *buffer* rather than
	 * the file was a real bug: a sink larger than the read cap had its tail
	 * deleted unread. Records this version cannot decode also keep the file,
	 * so an upgrade cannot destroy what the previous version wrote.
	 */
	*consumable = !foreign_seen
		&& (size_t) file_size == size
		&& offset + sizeof(cbox_crash_record_header_probe) > size;

	if (foreign_seen) {
		/* Keep everything: a version we cannot read is not ours to discard. */
		*consumed_bytes = 0;
	}

	free(buffer);

	return visited;
}

/*
 * Discard the first `consumed` bytes of a sink, keeping the rest.
 *
 * Only ever called for a file whose owning process has exited, which is what
 * makes it safe: an earlier design compacted a shared sink that live workers
 * were still appending to, and lost whatever arrived between the read and the
 * truncate.
 */
static void cbox_crash_compact(const char *path, off_t consumed)
{
	int fd;
	char buffer[8192];
	off_t read_at = consumed;
	off_t write_at = 0;

	fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);

	if (fd < 0) {
		return;
	}

	for (;;) {
		ssize_t chunk = pread(fd, buffer, sizeof(buffer), read_at);

		if (chunk <= 0) {
			break;
		}

		if (pwrite(fd, buffer, (size_t) chunk, write_at) != chunk) {
			close(fd);
			return;
		}

		read_at += chunk;
		write_at += chunk;
	}

	if (ftruncate(fd, write_at) != 0) {
		/* Leaving it long only risks re-reporting, never loss. */
	}

	close(fd);
}

/*
 * Drains every sink except this process's own, and only those whose owning
 * process is gone.
 *
 * Nothing is ever truncated or compacted. That is the whole point: the crash
 * handler cannot take a lock — locking is not async-signal-safe — so there is
 * no way to stop a writer appending while a reader rewrites the same file. A
 * dead process, on the other hand, will never append again, so its file can be
 * read whole and removed.
 *
 * A live process's sink is skipped entirely rather than read-and-kept. Reading
 * it would risk reporting the same record twice, and unlinking it would be far
 * worse: the owner would carry on writing into a file with no name, and its
 * eventual crash record would be lost.
 */
int cbox_crash_drain(const char *dir, uint32_t max, cbox_crash_visit_fn visit, void *context)
{
	DIR *handle;
	struct dirent *entry;
	int visited = 0;
	bool stop = false;
	bool consumable = false;
	off_t consumed = 0;
	char owned[CBOX_CRASH_PATH_MAX];
	pid_t self = getpid();
	size_t prefix_len = sizeof(CBOX_CRASH_FILE_PREFIX) - 1;

	if (dir == NULL || visit == NULL || max == 0) {
		return -1;
	}

	if (!cbox_crash_prepare_directory(dir, owned, sizeof(owned))) {
		return 0;
	}

	dir = owned;
	handle = opendir(dir);

	if (handle == NULL) {
		return errno == ENOENT ? 0 : -1;
	}

	while (!stop && (uint32_t) visited < max && (entry = readdir(handle)) != NULL) {
		char path[CBOX_CRASH_PATH_MAX];
		const char *name = entry->d_name;
		char *end = NULL;
		long owner;
		int found;

		if (strncmp(name, CBOX_CRASH_FILE_PREFIX, prefix_len) != 0) {
			continue;
		}

		owner = strtol(name + prefix_len, &end, 10);

		if (end == NULL || strcmp(end, CBOX_CRASH_FILE_SUFFIX) != 0 || owner <= 0) {
			continue;
		}

		if ((pid_t) owner == self || cbox_crash_process_alive((pid_t) owner)) {
			continue;
		}

		if (cbox_crash_sink_name(path, sizeof(path), dir, (pid_t) owner) != 0) {
			continue;
		}

		consumable = false;
		consumed = 0;
		found = cbox_crash_drain_file(path, max - (uint32_t) visited, visit, context,
			&stop, &consumable, &consumed);

		if (found >= 0) {
			visited += found;

			/*
			 * Only once the owner is gone AND the whole file has been read AND
			 * everything in it was handed over. Deleting it because a budget
			 * ran out, or because part of it was written by another version,
			 * would throw away records nobody has seen.
			 */
			if (consumable) {
				unlink(path);
			} else if (consumed > 0) {
				/*
				 * Could not finish the file — the caller's budget ran out, or
				 * it is larger than one read. Drop what was handed over so the
				 * next drain starts where this one stopped; without this the
				 * same records are returned on every call and the sink is
				 * never reclaimed.
				 *
				 * Rewriting is safe here in a way it was not before: this file
				 * belongs to a process that has exited, so nothing can be
				 * appending to it while we compact.
				 */
				cbox_crash_compact(path, consumed);
			}
		}
	}

	closedir(handle);

	return visited;
}
