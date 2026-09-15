---
title: "Testing against the extension"
weight: 12
description: "Writing tests for code that must work with and without the extension present."
---

# Testing against the extension

## The contract you are testing

Consuming code must behave identically whether or not the extension is loaded.
That is the whole design: `cbox_telemetry_begin()` returning `0` means "no
native telemetry", not "error", and every other entry point degrades the same
way.

So the test that matters most is the one that runs with the extension **absent**.

```php
$handle = extension_loaded('cbox_telemetry')
    ? cbox_telemetry_begin(['unit' => 'http', 'sampled' => $span->sampled])
    : 0;

try {
    return $next($request);
} finally {
    if ($handle !== 0) {
        $result = cbox_telemetry_finish($handle, includeProfile: $durationMs >= 500);
        // …
    }
}
```

`$handle !== 0` is the only guard you need — you do not also need
`extension_loaded()` at the end.

Be precise about what `0` means, though: it is returned only when the extension
is absent or switched off entirely (`cbox_telemetry.enabled=0`). A unit still
opens when profiling is unavailable — the handle is real, and `finish()` simply
returns `profiling => false` and `profile => null`. Do not read `0` as "no
profiling".

## What the extension guarantees, so you can assert it

- `cbox_telemetry_finish()` with an unknown or already-finished handle returns
  `[]`. It never throws and never warns.
- `cbox_telemetry_finish()` returns `profile => null` unless you passed
  `includeProfile: true` *and* the unit was actually profiled *and* at least one
  sample landed.
- `cbox_telemetry_drain_crashes()` returns `[]` when there is nothing to drain,
  including when the crash directory does not exist.
- Units do not nest. A second `begin()` abandons the first; the abandoned handle
  then returns `[]` from `finish()`.

## Testing crash handling

Never in the test process. Spawn a child, let it enter a unit of work, kill it,
then drain from the parent — `tests/015-crash-abort.phpt` in this repo is the
worked example. Point both processes at the same
`cbox_telemetry.crash.dir` via `-d`, not `ini_set()`: the setting is
`PHP_INI_SYSTEM` because it is read once, at startup, long before a request can
change it.

## Running this repo's own suite

```bash
make test                 # everything
make test TESTS=tests/004-accuracy.phpt
```

Tests that depend on an optional extension skip rather than fail, so a run with
some tests skipped on a build without OPcache or pcntl is a clean run.
