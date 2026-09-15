/*
 * cbox_telemetry — statistical CPU profiler.
 *
 * The timer signal never walks the PHP stack. It bumps a counter and sets the
 * VM interrupt flag; the engine then calls us back at a safe point, where the
 * walk actually happens and the pending ticks are folded into the call tree.
 * That is the difference between "profiler" and "source of impossible bug
 * reports".
 *
 * Samples cost no Zend allocation at all: names land in a bump arena, paths in
 * a fixed-size trie. A 30 second profile occupies the same memory as a 30 ms
 * one, and resetting between units of work is a pointer store.
 */
#ifndef CBOX_PROFILER_H
#define CBOX_PROFILER_H

#include "arena.h"
#include "frames.h"
#include "stacktree.h"

#define CBOX_PROFILER_DEPTH_HARD_MAX 256u

/*
 * Called from signal context by the timer backend. `ticks` is 1 plus the
 * timer's overrun count, so time spent where the VM could not be interrupted
 * (a long internal call, a blocking syscall) is still accounted for when
 * control comes back.
 *
 * Async-signal-safe: one relaxed atomic add and one atomic store.
 */
void cbox_profiler_tick(uint32_t ticks);

int  cbox_profiler_init(uint32_t max_frames, uint32_t max_nodes, size_t arena_bytes);
void cbox_profiler_shutdown(void);
bool cbox_profiler_ready(void);

/* Hook/unhook zend_interrupt_function. MINIT and MSHUTDOWN only. */
void cbox_profiler_install(void);
void cbox_profiler_uninstall(void);

/*
 * `max_duration_ns` is a safety valve for units nobody closes — an automatic
 * unit in a queue worker would otherwise sample for the life of the process.
 * Zero means no limit. When it trips, sampling stops and the samples taken so
 * far are kept; the unit itself stays open.
 */
int  cbox_profiler_start(uint64_t period_ns, uint32_t max_depth, uint64_t max_duration_ns);
void cbox_profiler_stop(void);
void cbox_profiler_reset(void);
bool cbox_profiler_running(void);

uint64_t cbox_profiler_period_ns(void);
uint64_t cbox_profiler_sample_count(void);
uint64_t cbox_profiler_dropped(void);
bool     cbox_profiler_capped(void);
size_t   cbox_profiler_arena_peak(void);

/* Read-only views for materialising the profile into PHP. */
const cbox_frame_table *cbox_profiler_frames(void);
const cbox_stack_tree  *cbox_profiler_tree(void);
const cbox_arena       *cbox_profiler_arena(void);

#endif /* CBOX_PROFILER_H */
