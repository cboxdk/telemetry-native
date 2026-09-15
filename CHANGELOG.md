# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/cboxdk/telemetry-native/compare/main...HEAD
