/*
 * cbox_telemetry — monotonic clock.
 *
 * Header-only so it can be called from the VM interrupt handler and from the
 * crash handler without a cross-TU call. clock_gettime() is on POSIX's
 * async-signal-safe list, which is what makes it usable in both.
 */
#ifndef CBOX_CLOCK_H
#define CBOX_CLOCK_H

#include <stdint.h>
#include <time.h>

static inline uint64_t cbox_now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}

	return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

static inline uint64_t cbox_realtime_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
		return 0;
	}

	return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

#endif /* CBOX_CLOCK_H */
