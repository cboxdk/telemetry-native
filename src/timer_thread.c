#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

/*
 * Timer-thread fallback — Darwin, and anything else without per-thread POSIX
 * CPU timers.
 *
 * The obvious fallback is setitimer(ITIMER_VIRTUAL). It does not survive
 * contact with Fibers: PHP switches the C stack with make_fcontext, and on
 * Darwin a signal delivered while the process is running on a fiber stack
 * corrupts the VM — reliably, and more often the faster you sample. An
 * alternate signal stack does not help.
 *
 * So this backend uses no signals at all. A dedicated thread sleeps for the
 * period and pokes the VM interrupt flag, exactly as the engine expects any
 * other thread to. The cost is that it measures *wall* time rather than CPU
 * time, which is why cbox_timer_is_cpu_time() returns false here and the
 * profile reports which clock it used.
 *
 * Linux is the production target and uses the CPU-time backend; this one only
 * has to be good enough to develop against.
 */

#include "timer.h"
#include "profiler.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_t       cbox_timer_thread;
static pthread_mutex_t cbox_timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cbox_timer_wake = PTHREAD_COND_INITIALIZER;

static bool     cbox_timer_installed = false;
static bool     cbox_timer_started = false;
static bool     cbox_timer_stopping = false;
static uint64_t cbox_timer_period_ns = 0;

/*
 * Read by the sampler thread after it wakes, written by the request thread when
 * a unit starts or ends. A plain bool touched from two threads is a data race
 * and therefore undefined behaviour, however benign it looks — the compiler is
 * entitled to assume it cannot change and hoist the check out of the loop.
 */
static volatile bool cbox_timer_is_armed = false;

static inline bool cbox_timer_armed_load(void)
{
	return __atomic_load_n(&cbox_timer_is_armed, __ATOMIC_ACQUIRE);
}

static inline void cbox_timer_armed_store(bool value)
{
	__atomic_store_n(&cbox_timer_is_armed, value, __ATOMIC_RELEASE);
}

static void cbox_timer_sleep(uint64_t nanoseconds)
{
	struct timespec requested;
	struct timespec remaining;

	requested.tv_sec = (time_t) (nanoseconds / 1000000000ull);
	requested.tv_nsec = (long) (nanoseconds % 1000000000ull);

	while (nanosleep(&requested, &remaining) != 0 && errno == EINTR) {
		requested = remaining;
	}
}

static void *cbox_timer_main(void *ignored)
{
	(void) ignored;

	for (;;) {
		uint64_t period;

		pthread_mutex_lock(&cbox_timer_lock);

		while (!cbox_timer_armed_load() && !cbox_timer_stopping) {
			pthread_cond_wait(&cbox_timer_wake, &cbox_timer_lock);
		}

		if (cbox_timer_stopping) {
			pthread_mutex_unlock(&cbox_timer_lock);
			break;
		}

		period = cbox_timer_period_ns;
		pthread_mutex_unlock(&cbox_timer_lock);

		cbox_timer_sleep(period);

		/*
		 * Re-check rather than trust the value we slept on: the unit may have
		 * finished while we were asleep, and a tick after that would be
		 * attributed to whatever runs next.
		 */
		if (cbox_timer_armed_load()) {
			cbox_profiler_tick(1);
		}
	}

	return NULL;
}

int cbox_timer_init(void)
{
	sigset_t block_all;
	sigset_t previous;
	int created;

	if (cbox_timer_installed) {
		return 0;
	}

	/*
	 * The sampler thread must never be chosen to handle a process-directed
	 * signal — PHP's own handlers expect to run on the main thread.
	 */
	sigfillset(&block_all);
	pthread_sigmask(SIG_SETMASK, &block_all, &previous);

	created = pthread_create(&cbox_timer_thread, NULL, cbox_timer_main, NULL);

	pthread_sigmask(SIG_SETMASK, &previous, NULL);

	if (created != 0) {
		return -1;
	}

	cbox_timer_started = true;
	cbox_timer_installed = true;

	return 0;
}

void cbox_timer_shutdown(void)
{
	if (!cbox_timer_installed) {
		return;
	}

	pthread_mutex_lock(&cbox_timer_lock);
	cbox_timer_stopping = true;
	cbox_timer_armed_store(false);
	pthread_cond_signal(&cbox_timer_wake);
	pthread_mutex_unlock(&cbox_timer_lock);

	if (cbox_timer_started) {
		pthread_join(cbox_timer_thread, NULL);
		cbox_timer_started = false;
	}

	cbox_timer_installed = false;
	cbox_timer_stopping = false;
}

int cbox_timer_arm(uint64_t period_ns)
{
	if (!cbox_timer_installed || period_ns == 0) {
		return -1;
	}

	pthread_mutex_lock(&cbox_timer_lock);
	cbox_timer_period_ns = period_ns;
	cbox_timer_armed_store(true);
	pthread_cond_signal(&cbox_timer_wake);
	pthread_mutex_unlock(&cbox_timer_lock);

	return 0;
}

void cbox_timer_disarm(void)
{
	pthread_mutex_lock(&cbox_timer_lock);
	cbox_timer_armed_store(false);
	pthread_mutex_unlock(&cbox_timer_lock);
}

bool cbox_timer_armed(void)
{
	return cbox_timer_armed_load();
}

void cbox_timer_after_fork(void)
{
	/*
	 * The sampler thread did not come across the fork. Whatever the mutex and
	 * condition variable looked like at that instant is frozen in the child —
	 * possibly locked by a thread that does not exist — so they are
	 * reinitialised rather than reused. The child is single-threaded here, so
	 * this is the one safe moment to do it.
	 */
	pthread_mutex_init(&cbox_timer_lock, NULL);
	pthread_cond_init(&cbox_timer_wake, NULL);

	cbox_timer_started = false;
	cbox_timer_installed = false;
	cbox_timer_stopping = false;
	cbox_timer_armed_store(false);
	cbox_timer_period_ns = 0;

	/* Start a fresh sampler thread for this process. */
	cbox_timer_init();
}

const char *cbox_timer_backend(void)
{
	return "thread-walltime";
}

int cbox_timer_signal_number(void)
{
	return 0; /* no signals are used by this backend */
}

bool cbox_timer_is_cpu_time(void)
{
	return false;
}
