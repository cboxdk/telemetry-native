# Cbox Telemetry Native

Deep PHP runtime telemetry for the Cbox stack: a statistical CPU profiler,
native timing for connection establishment and cURL, bounded runtime counters,
and a signal-safe crash recorder — in one optional extension.

It is the companion to [`cboxdk/laravel-telemetry`][laravel-telemetry], not a
replacement for it. `laravel-telemetry` owns everything semantic: trace context,
sampling policy, redaction, attribute naming, OTLP export. This extension only
observes and measures the PHP runtime. It knows nothing about Laravel, and it
never touches the network.

```bash
pie install cboxdk/telemetry-native
```

## Why

`laravel-telemetry` can tell you a request took 1,420 ms and burned 410 ms of
PHP CPU. It cannot tell you *which call stacks* burned it, why opening the
database connection took 180 ms, or what the process was doing when it
segfaulted. That is the boundary this extension crosses.

## What you get

```php
$handle = cbox_telemetry_begin([
    'trace_id' => $traceId,
    'span_id'  => $spanId,
    'unit'     => 'http',
    'sampled'  => true,
    'profile'  => true,
]);

// … the request or job happens …

$result = cbox_telemetry_finish($handle, includeProfile: $durationMs >= 500);
```

Or let it instrument itself and collect once at the end — which also means the
profile covers the framework booting, not just the part after your middleware
runs:

```ini
cbox_telemetry.auto = 1
```

```php
$result = cbox_telemetry_finish(0, includeProfile: $durationMs >= 500);
```

`$result` carries the unit's duration, per-type operation aggregates, runtime
counters, and — only when you ask for it — an aggregated CPU profile:

```php
[
    'duration_ns' => 1_842_000_000,
    'operations'  => ['pdo.connect' => ['count' => 1, 'total_ns' => 83_200_000, 'max_ns' => 83_200_000]],
    'counters'    => ['profiler.samples' => 814, 'profiler.dropped' => 0, 'gc.runs' => 3, …],
    'profile'     => [
        'sample_count'  => 814,
        'period_ns'     => 1_000_000,
        'clock'         => 'cpu',
        'frames'        => [['function' => 'App\\Services\\Pricing::calculate', 'file' => '…', 'line' => 82], …],
        'top_functions' => [['frame_id' => 1, 'samples' => 284], …],
        'stacks'        => null,
    ],
]
```

Frames are emitted once and referenced by id, so nothing repeats a string it has
already sent.

## Design notes worth knowing

**Sampling never runs in a signal handler.** The timer bumps a counter and sets
the VM interrupt flag; the engine calls back at a safe point, and the stack walk
happens there. Samples cost no Zend allocation at all — names go in a bump
arena, call paths into a fixed-size trie — so a 30-second profile takes the same
memory as a 30-millisecond one and resetting between jobs is a pointer store.

**A profile you don't ask for is never built.** Profiling can run for the whole
unit; if the caller decides afterwards that it was too fast to be interesting,
the native state is reset and no PHP array is ever allocated.

**A profile says how much to trust it.** Samples that could not be taken at a
safe point, and ticks the kernel never delivered, are counted separately and
reported with the profile — so a consumer can tell "94% of this was sampled
directly" from "most of this is arithmetic because the period is too fine".

**Everything is bounded.** Frames, call-tree nodes, breadcrumbs, the arena and
the operation nesting stack all have hard ceilings. Past them the extension
increments a dropped counter and carries on. It never grows, never blocks and
never fails the request.

**The crash handler does nothing risky.** It writes one fixed-width record to a
descriptor opened long beforehand, then restores the previous disposition and
lets the process die exactly as it would have — core dump and all. No
allocation, no Zend API, no PHP callbacks, no locks.

## Measured overhead

Under concurrent PHP-FPM load on the Cbox production image
(`ghcr.io/cboxdk/php-baseimages/php-fpm-nginx`), 8 workers on 4 CPUs, a request
that does CPU work, opens a PDO connection and runs 200 queries. CPU time is
read from the container's cgroup; scenarios are interleaved across rounds and
reported as medians.

| | CPU per request | vs baseline | p95 | worker RSS |
|---|---|---|---|---|
| baseline, no extension | 2.249 ms | — | 10 ms | 30,065 KB |
| loaded, never called | 2.238 ms | −0.5% | 9 ms | 30,348 KB |
| operation hooks armed | 2.279 ms | +1.3% | 9 ms | 30,396 KB |
| **profiling every request at 1 ms** | 2.336 ms | **+3.9%** | 10 ms | 30,459 KB |
| profiling every request at 200 µs | 2.414 ms | +7.3% | 10 ms | 30,512 KB |

Read it honestly: having the extension loaded costs nothing measurable, and
profiling *every* request at 1 ms costs about 4% CPU — a little above the 3%
this set out to hit. Profiling only sampled requests costs proportionally less,
and that is the intended deployment.

The 200 µs row is the interesting one. It costs nearly twice as much and buys
nothing: at that period the kernel cannot deliver, and `timer_overruns` reports
1,194 of 1,492 ticks skipped. Faster is not more accurate, which is why the
default is 1 ms.

Memory is flat under sustained load — +8 KB per worker across 60 seconds at
concurrency 8.

```bash
benchmarks/fpm/load.sh 4000 8 8.4 3 60
```

Two measurement mistakes are recorded in that script's comments, because both
produced confident nonsense: requests-per-second is noisy enough to rank
"loaded but never called" as *slower* than "loaded and hooking", and summing
per-worker CPU from `/proc` goes backwards when FPM recycles workers. Running
scenarios in sequence rather than interleaved made the extension look 8.5% more
expensive than baseline purely by being measured later.

## Tested alongside

Native extensions share signals, handlers and the function table, so
coexistence is verified rather than assumed:

| | result |
|---|---|
| Xdebug (develop, trace) | works; samples rise because Xdebug slows execution |
| OPcache, including tracing JIT | works |
| Excimer profiling simultaneously | works — different signals, both collect |
| PHP-FPM, root master + non-root pool | works; `tests/fpm/run.sh` |

Commercial agents (Datadog, New Relic, Tideways) are **not** in that list. They
need licences and live accounts, so nothing here has been verified against them
and this documentation will not pretend otherwise. The extension restores a
handler only when it is still its own, which is the behaviour that should make
it a good citizen, but "should" is not "tested".

## Support

| | |
|---|---|
| PHP | 8.3, 8.4, 8.5 |
| Linux | x86_64 and arm64, glibc and musl — the production target |
| macOS | arm64, development only. Profiling is off by default there — see [KNOWN-ISSUES.md](KNOWN-ISSUES.md); everything else works |
| Windows | out of scope |
| ZTS | not supported; the code stays TSRM-clean for when it is |

## Documentation

- [Installation](docs/getting-started/installation.md)
- [How the profiler works](docs/core-concepts/profiler.md)
- [Native operation timing](docs/core-concepts/operations.md)
- [Crash records](docs/core-concepts/crashes.md)
- [Configuration reference](docs/configuration/reference.md)
- [Security and privacy](docs/security/_index.md)
- [Decisions](docs/decisions/0001-operation-hook-mechanism.md)

## License

MIT. See [LICENSE.md](LICENSE.md).

This is a clean-room implementation. No code, structure or comments were taken
from Excimer or any other GPL-licensed profiler.

[laravel-telemetry]: https://github.com/cboxdk/laravel-telemetry
