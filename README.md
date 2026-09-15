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

**Everything is bounded.** Frames, call-tree nodes, breadcrumbs, the arena and
the operation nesting stack all have hard ceilings. Past them the extension
increments a dropped counter and carries on. It never grows, never blocks and
never fails the request.

**The crash handler does nothing risky.** It writes one fixed-width record to a
descriptor opened long beforehand, then restores the previous disposition and
lets the process die exactly as it would have — core dump and all. No
allocation, no Zend API, no PHP callbacks, no locks.

## Measured overhead

On PHP 8.4/Linux, against the same build without the extension. Note that the
containerised environment these were taken in has a noise floor of roughly ±4%,
so treat everything below it as "not measurable here" rather than as a precise
figure — the numbers that mattered were far outside it (see
[decision 0001](docs/decisions/0001-operation-hook-mechanism.md)).

| mode | cpu | calls | deep | internal | mixed |
|---|---|---|---|---|---|
| loaded, never called | -3.0% | 3.8% | 1.3% | -3.2% | -1.5% |
| operation hooks armed | 4.6% | 5.8% | 0.6% | -0.5% | -2.8% |
| begin/finish per unit | 0.3% | 1.6% | 0.0% | -2.1% | -1.9% |
| profiling at 1 ms | 2.4% | 4.9% | -1.9% | 1.4% | 3.5% |
| everything on | 1.7% | 5.2% | 2.0% | 0.3% | -1.8% |

Reproduce with `php benchmarks/run.php modules/cbox_telemetry.so 21 1000`.

## Support

| | |
|---|---|
| PHP | 8.3, 8.4, 8.5 |
| Linux | x86_64 and arm64, glibc and musl — the production target |
| macOS | arm64, development only: wall-clock sampling, not CPU time |
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
