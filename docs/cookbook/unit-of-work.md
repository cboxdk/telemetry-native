---
title: "Bracketing a unit of work"
weight: 41
description: "The integration pattern for a request or job, including the ordering details that are easy to get wrong."
---

# Bracketing a unit of work

## The pattern

```php
$handle = extension_loaded('cbox_telemetry')
    ? cbox_telemetry_begin([
        'trace_id' => $span->traceId,
        'span_id'  => $span->spanId,
        'unit'     => 'http',
        'sampled'  => $span->sampled,
        'profile'  => true,
    ])
    : 0;

try {
    return $next($request);
} finally {
    if ($handle !== 0) {
        $durationMs = (hrtime(true) - $startedAt) / 1e6;

        $result = cbox_telemetry_finish(
            $handle,
            includeProfile: $durationMs >= 500,
        );

        $telemetry->recordNativeResult($result);
    }
}
```

`$handle !== 0` is the whole guard. A `0` means "no native telemetry" for any
reason — extension absent, disabled, profiler unavailable — and the calling code
does not need to distinguish them.

## Start profiling always, keep it rarely

Profiling for every sampled unit and discarding most results is cheaper than it
sounds, and much cheaper than the alternative. The sampler's cost is roughly
constant; deciding *afterwards* whether a unit was interesting requires no
prediction, and prediction is the expensive part.

A fast unit resets the native state and allocates nothing into PHP. Only
`includeProfile: true` builds an array.

## Two ordering details

**Finish before you close the span.** Operation aggregates and counters come
back from `finish()`, so anything that attaches them to a span has to happen
while the span is still open. Compute the elapsed time yourself rather than
asking a span that has not ended yet.

**Re-check the sampling decision at the end.** If the caller can change its mind
mid-request — a sampler that promotes slow or failing requests, for instance —
the decision that matters is the one in force at `finish()`. If the unit ended
up unsampled, just do not ask for the profile; the native state is reset either
way and nothing is allocated.

## Long-running workers

Call `finish()` for every unit, including ones that fail. If a fatal error takes
the request out, the extension still cleans up at RSHUTDOWN — the timer is
disarmed, the profile is reset, the unit is cleared — so a leaked handle cannot
bleed into the next job. Do not rely on that as the normal path, though; it is a
backstop, not a design.

Units do not nest. If your code can begin a unit while one is already running,
that is a bug in the caller: the first unit is abandoned, and its handle then
returns `[]`.

## Draining crash records

Records are written by processes that no longer exist, so something else has to
collect them — the next request, a scheduled flush, a dedicated command:

```php
foreach (cbox_telemetry_drain_crashes(32) as $record) {
    $telemetry->recordCrash($record);
}
```

Draining consumes, so exactly one collector should run at a time, and it should
run somewhere failure is visible. A crash record that is drained and then
dropped on the floor is worse than one still sitting in the sink.
