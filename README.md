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
never fails the request. Resetting between units is cheap but not free: the
arena is a pointer store, the hash buckets are a memset of roughly 160 KB at
the default limits.

**The crash handler does nothing risky.** It writes one fixed-width record,
then restores the previous disposition and re-raises, so the process dies
exactly as it would have — core dump and all. It opens the sink itself, which
is safe because `open()` and `write()` are both on POSIX's async-signal-safe
list; the path is built beforehand because `snprintf` is not. No allocation, no
Zend API, no PHP callbacks, no locks.

## Measured overhead

Under concurrent PHP-FPM load on the Cbox production image
(`ghcr.io/cboxdk/php-baseimages/php-fpm-nginx`), 8 workers on 4 CPUs, a request
that does CPU work, opens a PDO connection and runs 200 queries. CPU time is
read from the container's cgroup; scenario order is shuffled per round and
reported as medians over 19 rounds of 6,000–8,000 requests.

The second row is the point of the table: it is the **baseline again**, same
configuration, measured in the same run. Whatever it differs from the first row
by is measurement error, because nothing about it is different.

| | CPU per request | vs baseline | worker RSS |
|---|---|---|---|
| baseline, no extension | 2.471 ms | — | 30,005 KB |
| **baseline again, identical config** | 2.529 ms | **+1.6%** | — |
| loaded, all hooks off, crash off | 2.498 ms | within noise | 30,324 KB |
| loaded, hooks installed, crash armed | 2.513 ms | within noise | 30,445 KB |
| profiling every request at 1 ms | 2.574 ms | within noise (+2%, CI −4…+12%) | 30,539 KB |
| profiling every request at 200 µs | 2.504 ms | within noise | 30,540 KB |

p95 latency was 11–12 ms in every row, baseline included.

Differences smaller than about **6%** are not meaningful here — that is what
the repeated baseline says, with a 95% interval of −4% to +6% across 19
interleaved rounds, and single rounds swinging 20–40%. No row in this table is
outside that band, including the profiling rows.

So the honest reading is: **loading the extension, installing the hooks and
arming the crash recorder cost nothing this harness can resolve, and profiling
every request costs somewhere between nothing and about 5%.** An earlier, much
shorter run of this same script reported +3.9% and +7.3% for the two profiling
rows; a longer run with a control does not reproduce them, and they should not
be quoted. On single-process workloads (`benchmarks/run.php`), where there is
no scheduler noise to hide in, the same profile costs 0–5% on tight loops.

This is measured on a Linux VM on an Apple laptop, and the noise is dominated
by the host rather than by the harness — most likely by which cores the vCPUs
land on. On a real Linux host the floor should be lower; regenerate the table
there before quoting it in a capacity plan.

What the microbenchmarks can resolve, where the FPM harness cannot:

| | |
|---|---|
| Telemetry added per hooked call | ~36 ns — two clock reads, two breadcrumbs, and the operation token |
| Added per request by fork detection | ~0.2 µs (two `getpid()` calls), 0.008% of a 2.4 ms request |
| Materialising a retained ~70-sample profile | 0.1–0.3 ms |
| Discarding a profile instead | free within measurement resolution |
| Functions that are *not* hooked | unaffected — the handler swap is per function, not global |

The 200 µs row is the one worth understanding. It does not cost measurably more
than 1 ms, and it cannot: `timer_overruns` reported 1,194 of 1,492 ticks
skipped. Linux evaluates POSIX CPU timers on the scheduler tick, so on the
`CONFIG_HZ=1000` kernel these were taken on, roughly 1 ms is the floor no
matter what you ask for — asking for 200 µs delivers the same ~1,000
interrupts per second and simply reports four of five as skipped. On a `HZ=250`
kernel — the Debian/Ubuntu generic default — even 1 ms would overrun three
ticks in four. Ask for a period your kernel can actually deliver, and read
`timer_overruns` to find out what that is.

Memory is flat under sustained load — +8 KB per worker across 60 seconds at
concurrency 8.

```bash
benchmarks/fpm/load.sh 4000 8 8.4 3 60
```

Run it with few rounds and watch the control row: at two rounds it reported
the baseline as 8.0% *faster* than itself. That number is the harness telling
you how much of the table you are allowed to believe, and it is the reason the
profiling rows here are quoted as "within noise" rather than as the +3.9% an
earlier short run produced.

Three measurement mistakes are recorded in that script's comments, because all
three produced confident nonsense: requests-per-second is noisy enough to rank
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

| Xdebug (develop, trace) | tried by hand during development; **not** covered by a test |
| Excimer profiling simultaneously | same — tried once, not covered by a test |

Only the OPcache/JIT and PHP-FPM rows have tests behind them (`tests/011-jit.phpt`,
`tests/fpm/run.sh`). The other two were checked by hand and could regress
without anything noticing.

Commercial agents (Datadog, New Relic, Tideways) are **not** in that list at
all. They need licences and live accounts, so nothing here has been verified
against them.

One asymmetry worth knowing: *function* handlers are restored only if they are
still ours, so an agent that wraps us afterwards keeps working. *Signal*
dispositions are restored unconditionally at shutdown, so a handler installed
after ours is dropped.

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
