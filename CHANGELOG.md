# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-09-15

### Added

- Statistical CPU profiler. The timer never walks the stack itself: it bumps a
  counter and sets the VM interrupt flag, and the engine calls back at a safe
  point where the walk actually happens. Samples cost no Zend allocation —
  names land in a bump arena, call paths in a fixed-size trie — so profile
  memory is a function of the code's shape, not of how long you profiled.
- Tail retention. Profiling can run for a whole unit of work and still allocate
  nothing into PHP if the caller decides afterwards it was too fast to be worth
  keeping.
- Native operation timing for `PDO::__construct`, `PDO::connect`,
  `Redis::connect`, `Redis::pconnect`, `curl_exec`, and optionally
  `stream_socket_client` / `fsockopen`, aggregated by type.
- Runtime counters: sample and drop counts, GC runs and collections per unit,
  breadcrumb and buffer-overflow counters, arena high-water mark.
- Crash recorder: a bounded breadcrumb ring, a versioned fixed-width binary
  record, handlers for `SIGSEGV`/`SIGABRT`/`SIGBUS`/`SIGILL` that allocate
  nothing and chain to whatever was there before, and a drain that decodes
  records back into PHP.
- Optional automatic instrumentation (`cbox_telemetry.auto`, off by default).
  A unit of work opens at RINIT, so measurement starts at the first instruction
  rather than whenever a framework gets around to asking — which is the only way
  to profile autoloading, service providers and config, all of which happen long
  before any middleware runs. `cbox_telemetry_finish(0)` collects whichever unit
  is open, so a terminate hook needs no handle. The first `begin()` *adopts* the
  running unit instead of restarting it, keeping the bootstrap samples and
  labelling them with the caller's trace context. `auto_max_ms` stops an
  automatic unit that nobody closes from sampling forever in a long-running
  process, where explicit begin/finish per job is the right shape anyway.
- Five PHP functions and nothing else: `cbox_telemetry_version`,
  `cbox_telemetry_status`, `cbox_telemetry_begin`, `cbox_telemetry_finish`,
  `cbox_telemetry_drain_crashes`.

### Fixed before the first release

- **A drain could delete crash records nobody had read.** When the caller's
  record budget ran out mid-file, the file was unlinked anyway. It is now
  removed only once it has been read to the end.
- **`auto_max_ms` was never clamped**, so `0` silently disabled the safety
  valve and a negative value wrapped into a deadline centuries away — in the
  one setting whose entire job is to stop an unattended unit sampling forever.
- **Crash sinks are created on a crash, not on every request.** Every process
  used to leave a zero-byte file behind; on a recycling FPM pool that is
  thousands of empty files a day in a shared directory. The handler creates the
  file itself, which is safe because `open()` and `write()` are.
- **`profiler_status` said `unknown`** when the extension was disabled outright,
  which is the one case where the answer is obvious.

### Platform safety

- **Profiling is disabled by default where there is no per-thread CPU timer**
  (in practice macOS). Interrupting the VM from a sampler thread corrupts it:
  2 crash records over 20 suite runs with the cross-thread interrupt store,
  0 with the profiler off, 0 with the profiler running and only that store
  removed. A captured record pinned the fault to a NULL `zend_execute_data`
  dereferenced at its `func` field, inside the PHP binary rather than this
  extension. Delivering the interrupt by `pthread_kill` instead was worse
  (13 records over 40 runs), because it can land the thread anywhere, while
  Linux's CPU-time timer fires only while that thread executes PHP.
  `cbox_telemetry_status()` reports the reason, and
  `profiler.allow_fallback_backend=1` overrides it. Linux is unaffected, and
  everything other than sampling keeps working everywhere.

### Fixed after review

- **Two closures in one file were one frame.** Frame identity was name plus
  file; before PHP 8.4 every closure in a file is called `{closure}`, so their
  samples were all reported against whichever was seen first. Identity now
  includes the declaration line.
- **A forked child profiled nothing while reporting that it was.** POSIX
  per-thread timers are not inherited across `fork()`, and neither are threads,
  but the copied state said otherwise. The extension now notices the pid change
  and rebuilds the timer, the sampler thread, the inherited unit and the crash
  sink descriptor.
- **The crash recorder was silently dead under standard PHP-FPM.** The sink was
  created during module startup, which runs in the root master, leaving a
  root-owned directory no worker could write. Sinks are now opened per process
  on the first request, under the identity that writes them. Covered by
  `tests/fpm/run.sh` against a real FPM master with a non-root pool.
- **Draining could lose a crash record.** Compacting a shared file meant
  truncating it, and anything appended between the last read and the truncate
  was gone — a window the handler cannot close, because locking is not
  async-signal-safe. Each process now has its own sink and a drain only
  consumes files whose owner has exited.
- **A crash could capture a half-written breadcrumb.** Entries are now
  published with their sequence number last, so a slot being written is skipped
  rather than reported as a mixture of two operations.
- **One lost sample could count as several drops.** `profiler.dropped` is now
  exactly "sampling events that could not be represented"; the capacity
  counters that explain why are reported separately.
- **The profiler's duration cap could never fire.** It tested the sample
  counter for an exact round number, but that counter advances by however many
  ticks were booked at once, so it steps straight over the checkpoint. An
  automatic unit in a long-running process would have sampled forever — which
  is the one thing the cap exists to prevent. Found by its own test flaking on
  PHP 8.5 rather than by reading the code.
- **The macOS timer had a data race.** The armed flag was read by the sampler
  thread outside the mutex that guarded its writes. Now atomic.

### Added after review

- **Profiler confidence.** `deferred_samples`, `deferred_events`,
  `max_deferred` and `timer_overruns`, on the profile and in the counters.
  Deferred means the VM was in a long internal call; overruns mean the kernel
  could not deliver at the requested period and nothing was observed at all.
  They are counted separately because one is a fact about the application and
  the other about the configuration. At 200 µs on Linux, 1,194 of 1,492 ticks
  turn out to be overruns — previously invisible.
- **Truthful capability reporting.** `hook_detail` reports requested versus
  installed versus unavailable per group, `hooks_installed` names the functions
  actually wrapped, and `profiler_status` says why profiling is degraded rather
  than only that it is off.
- **Load and soak harness on the production image**
  (`benchmarks/fpm/load.sh`), measuring CPU per request from the container
  cgroup under concurrent PHP-FPM. Loaded-but-uncalled is unmeasurable;
  profiling every request at 1 ms costs ~3.9% CPU; memory is flat under
  sustained load.
- **End-to-end benchmarks** that include `finish()`, measured separately for
  retained and discarded profiles, plus a long-running worker memory benchmark.

### Notes on what the testing changed

- **Operation hooks swap internal handlers rather than registering a Zend
  Observer.** Registering any observer puts the whole engine on its
  observed-call path: measured ~20% slower on call-heavy workloads that never
  call an observed function. The handler swap is unmeasurable against noise.
  See `docs/decisions/0001-operation-hook-mechanism.md`.
- **Nothing uses `SIGPROF` or `ITIMER_PROF`.** They are how PHP implements
  `max_execution_time` on builds without `ZEND_MAX_EXECUTION_TIMERS`; sharing
  them turned every sample into "Maximum execution time of 0 seconds exceeded".
  Linux samples on `SIGRTMIN + 4`.
- **macOS uses a timer thread and no signals at all.** Delivering a signal while
  PHP is running on a Fiber's C stack crashes the VM on Darwin, reliably and in
  proportion to the sampling rate. The fallback backend samples wall clock
  instead of CPU time and says so, in `status()` and in every profile.

[Unreleased]: https://github.com/cboxdk/telemetry-native/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/cboxdk/telemetry-native/releases/tag/v0.1.0
