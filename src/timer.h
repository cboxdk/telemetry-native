/*
 * cbox_telemetry — sampling timer.
 *
 * Two backends, chosen by config.m4:
 *
 *   timer_posix.c   Linux. timer_create(CLOCK_THREAD_CPUTIME_ID) with
 *                   SIGEV_THREAD_ID, so the timer measures *this thread's*
 *                   CPU time and the signal is delivered to this thread.
 *                   Delivered on a real-time signal.
 *   timer_itimer.c  macOS and anything else without SIGEV_THREAD_ID.
 *                   setitimer(ITIMER_VIRTUAL): user CPU time, process-wide and
 *                   much coarser. Dev-grade, not a production target.
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

/* Install the SIGPROF handler, remembering any handler already there. */
int  cbox_timer_init(void);
void cbox_timer_shutdown(void);

/* 0 on success. Arming an armed timer re-arms it at the new period. */
int  cbox_timer_arm(uint64_t period_ns);
void cbox_timer_disarm(void);
bool cbox_timer_armed(void);

const char *cbox_timer_backend(void);

/* Which signal this backend delivers on — reported by status() for diagnostics. */
int cbox_timer_signal_number(void);

/* False when the backend can only approximate CPU time. */
bool cbox_timer_is_cpu_time(void);

#endif /* CBOX_TIMER_H */
