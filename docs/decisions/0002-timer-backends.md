---
title: "0002 — Two timer backends, and no SIGPROF"
weight: 2
description: "Why Linux samples on a real-time signal, macOS uses a timer thread, and neither goes near SIGPROF."
---

# 0002 — Two timer backends, and no SIGPROF

Status: accepted, 2026-09-15.

## Context

A statistical profiler needs something to interrupt PHP at a fixed rate. The
obvious POSIX answers are `setitimer(ITIMER_PROF)` and
`timer_create(CLOCK_THREAD_CPUTIME_ID)`, both delivering a signal.

Two things went wrong with the obvious answers, and both were found by running
the code rather than by reading about it.

## SIGPROF belongs to PHP

PHP implements `max_execution_time` with `setitimer(ITIMER_PROF)` and `SIGPROF`
on every build without `ZEND_MAX_EXECUTION_TIMERS` — which includes the stock
Debian PHP images. There is exactly one `ITIMER_PROF` and one `SIGPROF` handler
per process.

Sharing them does not degrade gracefully. The first symptom was every profiled
script dying with:

```
Fatal error: Maximum execution time of 0 seconds exceeded
```

because PHP's timeout handler was receiving our sampling ticks.

**Decision:** never use `SIGPROF` or `ITIMER_PROF`. On Linux we deliver on a
real-time signal (`SIGRTMIN + 4`, falling back to `SIGRTMIN`), which nothing in
the engine claims — PHP's own execution timer uses `SIGRTMIN` itself when it has
one. `cbox_telemetry_status()` reports the signal number so a conflict is
visible rather than mysterious.

## Signals and Fibers do not mix on Darwin

With `setitimer(ITIMER_VIRTUAL)` on macOS, any script using Fibers crashed —
reliably, and more often the faster we sampled:

| period | outcome (8 runs) |
|---|---|
| 500 µs | 8 ok |
| 200 µs | 2 ok, 6 crashed |
| 100 µs | 0 ok, 8 crashed |

The crash is inside the VM (`EX(call)` observed NULL at `ZEND_DO_UCALL`), not in
our code. It reproduces with the stack walk disabled, with the VM-interrupt flag
never set, and with a single fiber that never suspends — so the trigger is the
*delivery of a signal while the process is running on a fiber's C stack*, which
PHP switches underneath us with `make_fcontext`. An alternate signal stack does
not help. The same code on Linux is clean at every rate we tried.

**Decision:** on Darwin, use no signals at all. A dedicated thread sleeps for
the period and sets the VM interrupt flag, the same way any other thread would.

## Backends

| | Linux | Darwin and other fallbacks |
|---|---|---|
| mechanism | `timer_create(CLOCK_THREAD_CPUTIME_ID)`, `SIGEV_THREAD_ID` | timer thread + `nanosleep` |
| signal | `SIGRTMIN + 4` | none |
| clock | thread CPU time | wall clock |
| accuracy | one scheduler tick (≈1 ms at `CONFIG_HZ=1000`); finer requests are reported as overruns | period + scheduling latency |
| status name | `posix-thread-cputime` | `thread-walltime` |

Linux is the production target and gets true CPU-time sampling, accurate to the
requested period. The fallback only has to be good enough to develop against,
and it is honest about the difference: `cbox_telemetry_status()` reports
`timer_cpu_time = false`, and every profile carries `clock => "wall"` rather
than implying a CPU measurement it did not make.

## Consequences

- Sampling on the fallback backend is wall-clock, so time blocked on I/O shows
  up as samples. That is arguably more useful when debugging locally and plainly
  wrong for CPU attribution, which is why production is Linux-only.
- The timer thread blocks all signals before it starts, so it can never be
  chosen to handle a process-directed signal meant for PHP's main thread.
- There is no portable spelling for the target thread id in `struct sigevent`:
  glibc wants `_sigev_un._tid`, musl and the kernel headers spell it
  `sigev_notify_thread_id`. `configure` probes all three.
