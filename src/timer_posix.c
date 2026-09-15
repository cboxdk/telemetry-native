#ifndef _GNU_SOURCE
# define _GNU_SOURCE 1
#endif

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "timer.h"
#include "profiler.h"
#include "sigstack.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef HAVE_GETTID
# include <sys/syscall.h>
static inline pid_t cbox_gettid(void)
{
	return (pid_t) syscall(SYS_gettid);
}
#else
static inline pid_t cbox_gettid(void)
{
	return gettid();
}
#endif

static timer_t          cbox_timer;
static struct sigaction cbox_prev_action;
static bool             cbox_timer_created = false;
static bool             cbox_timer_installed = false;
static bool             cbox_timer_is_armed = false;
static int              cbox_signal = 0;

/*
 * Never SIGPROF: PHP uses it for max_execution_time whenever the build has no
 * ZEND_MAX_EXECUTION_TIMERS, and our samples would be read as request
 * timeouts. Real-time signals are queued per-timer and nothing in the engine
 * claims this far up the range — SIGRTMIN itself is where PHP puts its own
 * execution timer when it has one.
 */
static int cbox_pick_signal(void)
{
#if defined(SIGRTMIN) && defined(SIGRTMAX)
	if (SIGRTMIN + 4 <= SIGRTMAX) {
		return SIGRTMIN + 4;
	}

	if (SIGRTMIN <= SIGRTMAX) {
		return SIGRTMIN;
	}
#endif

	return SIGPROF;
}

static void cbox_timer_chain(int sig, siginfo_t *info, void *context)
{
	if (cbox_prev_action.sa_flags & SA_SIGINFO) {
		if (cbox_prev_action.sa_sigaction != NULL) {
			cbox_prev_action.sa_sigaction(sig, info, context);
		}

		return;
	}

	if (cbox_prev_action.sa_handler != SIG_DFL && cbox_prev_action.sa_handler != SIG_IGN) {
		cbox_prev_action.sa_handler(sig);
	}
}

/*
 * Signal context. Async-signal-safe only: a tagged comparison and two atomic
 * stores. Anything else — allocation, the Zend API, stdio — would be a bug.
 */
static void cbox_timer_signal(int sig, siginfo_t *info, void *context)
{
	if (cbox_timer_created
		&& info != NULL
		&& info->si_code == SI_TIMER
		&& info->si_value.sival_ptr == (void *) &cbox_timer
	) {
		uint32_t ticks = 1;

#ifdef CBOX_HAVE_SI_OVERRUN
		/*
		 * The kernel queues one signal per timer and counts the rest. Without
		 * this, every millisecond spent inside a long internal call would
		 * collapse into a single sample.
		 */
		if (info->si_overrun > 0) {
			ticks += (uint32_t) info->si_overrun;
		}
#endif

		cbox_profiler_tick(ticks);

		return;
	}

	/* Not ours — another timer, or somebody's kill. Pass it on. */
	cbox_timer_chain(sig, info, context);
}

int cbox_timer_init(void)
{
	struct sigaction action;

	if (cbox_timer_installed) {
		return 0;
	}

	cbox_signal = cbox_pick_signal();

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = cbox_timer_signal;
	action.sa_flags = SA_SIGINFO | SA_RESTART;

	/*
	 * Fibers switch the C stack out from under us. A tick that lands mid-switch
	 * must not run on whatever the stack pointer happens to be at that instant.
	 */
	if (cbox_sigstack_ensure()) {
		action.sa_flags |= SA_ONSTACK;
	}

	sigemptyset(&action.sa_mask);

	if (sigaction(cbox_signal, &action, &cbox_prev_action) != 0) {
		cbox_signal = 0;
		return -1;
	}

	cbox_timer_installed = true;

	return 0;
}

void cbox_timer_shutdown(void)
{
	cbox_timer_disarm();

	if (cbox_timer_created) {
		timer_delete(cbox_timer);
		cbox_timer_created = false;
	}

	if (cbox_timer_installed) {
		sigaction(cbox_signal, &cbox_prev_action, NULL);
		cbox_timer_installed = false;
		cbox_signal = 0;
	}
}

static int cbox_timer_create_once(void)
{
	struct sigevent event;

	if (cbox_timer_created) {
		return 0;
	}

	memset(&event, 0, sizeof(event));
	event.sigev_notify = SIGEV_THREAD_ID;
	event.sigev_signo = cbox_signal;
	event.sigev_value.sival_ptr = (void *) &cbox_timer;
	/* Spelling resolved by configure — see config.m4. */
	event.CBOX_SIGEV_TID_FIELD = cbox_gettid();

	if (timer_create(CLOCK_THREAD_CPUTIME_ID, &event, &cbox_timer) != 0) {
		return -1;
	}

	cbox_timer_created = true;

	return 0;
}

int cbox_timer_arm(uint64_t period_ns)
{
	struct itimerspec spec;

	if (!cbox_timer_installed || period_ns == 0) {
		return -1;
	}

	if (cbox_timer_create_once() != 0) {
		return -1;
	}

	spec.it_interval.tv_sec = (time_t) (period_ns / 1000000000ull);
	spec.it_interval.tv_nsec = (long) (period_ns % 1000000000ull);
	spec.it_value = spec.it_interval;

	if (timer_settime(cbox_timer, 0, &spec, NULL) != 0) {
		return -1;
	}

	cbox_timer_is_armed = true;

	return 0;
}

void cbox_timer_disarm(void)
{
	struct itimerspec spec;

	if (!cbox_timer_created) {
		cbox_timer_is_armed = false;
		return;
	}

	memset(&spec, 0, sizeof(spec));
	timer_settime(cbox_timer, 0, &spec, NULL);
	cbox_timer_is_armed = false;
}

bool cbox_timer_armed(void)
{
	return cbox_timer_is_armed;
}

void cbox_timer_after_fork(void)
{
	/*
	 * Do not timer_delete() — the child never had the timer, and the handle is
	 * meaningless here. Signal dispositions *are* inherited, so the handler
	 * stays installed and only the timer needs recreating on next arm.
	 */
	cbox_timer_created = false;
	cbox_timer_is_armed = false;
}

const char *cbox_timer_backend(void)
{
	return "posix-thread-cputime";
}

int cbox_timer_signal_number(void)
{
	return cbox_signal;
}

bool cbox_timer_is_cpu_time(void)
{
	return true;
}
