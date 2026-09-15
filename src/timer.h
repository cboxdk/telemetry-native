/*
 * cbox_telemetry — sampling timer.
 *
 * Two backends, chosen by config.m4:
 *
 *   timer_posix.c   Linux. timer_create(CLOCK_THREAD_CPUTIME_ID) with
 *                   SIGEV_THREAD_ID, so the timer measures *this thread's*
 *                   CPU time and the signal is delivered to this thread.
 *                   Delivered on a real-time signal.
 *   timer_thread.c  macOS and anything else without SIGEV_THREAD_ID. A sampler
 *                   thread, wall clock, and unsafe enough that profiling is
 *                   off by default there — see KNOWN-ISSUES.md.
 *
 * Neither backend uses SIGPROF or ITIMER_PROF, and that is deliberate: PHP
 * implements max_execution_time with exactly those on builds without
 * ZEND_MAX_EXECUTION_TIMERS. Sharing them makes every sample look like a
 * request timeout.
 *
 * The handler is the only code that runs in signal context and it does exactly
 * two things — see cbox_profiler_tick().
 */
#ifndef CBOX_TIMER_H
#define CBOX_TIMER_H

#include <stdbool.h>
#include <stdint.h>

/* Install the sampling handler, remembering any handler already there. */
int  cbox_timer_init(void);
void cbox_timer_shutdown(void);

/* 0 on success. Arming an armed timer re-arms it at the new period. */
int  cbox_timer_arm(uint64_t period_ns);
void cbox_timer_disarm(void);
bool cbox_timer_armed(void);

/*
 * Re-establish the timer in a forked child.
 *
 * Neither backend survives fork(): POSIX per-thread timers are explicitly not
 * inherited, and neither are threads. The child wakes up holding state that
 * says a timer exists when none does, and would sample nothing forever while
 * reporting that profiling is on.
 */
void cbox_timer_after_fork(void);

const char *cbox_timer_backend(void);

/* Which signal this backend delivers on — reported by status() for diagnostics. */
int cbox_timer_signal_number(void);

/* False when the backend can only approximate CPU time. */
bool cbox_timer_is_cpu_time(void);

#endif /* CBOX_TIMER_H */
