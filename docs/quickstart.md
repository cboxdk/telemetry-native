---
title: "Quickstart"
weight: 2
description: "Install the extension and read your first profile, in one page."
---

# Quickstart

## Install

```bash
pie install cboxdk/telemetry-native
```

Check it is there and sane:

```bash
php -r 'print_r(cbox_telemetry_status());'
```

The two fields worth reading first:

```
[timer_backend] => posix-thread-cputime   # CPU-time sampling — the good one
[timer_cpu_time] => 1
```

`thread-walltime` instead means you are on a fallback backend (macOS) that
samples wall clock. Fine for development, not a production target.

## Profile something

```php
<?php

function inner(): float
{
    $total = 0.0;

    for ($i = 1; $i < 3000; $i++) {
        $total += sqrt($i);
    }

    return $total;
}

function outer(int $rounds): void
{
    for ($i = 0; $i < $rounds; $i++) {
        inner();
    }
}

$handle = cbox_telemetry_begin(['unit' => 'http', 'profile' => true]);

outer(400);

$result = cbox_telemetry_finish($handle, includeProfile: true);
$profile = $result['profile'];

foreach (array_slice($profile['top_functions'], 0, 5) as $entry) {
    $frame = $profile['frames'][$entry['frame_id']];
    printf("%6d  %s\n", $entry['samples'], $frame['function']);
}
```

```
   284  sqrt
   142  inner
     3  outer
```

`outer` barely appears, which is the point: these are *self* samples. `outer`
spends its time waiting for `inner`, and a profiler that blamed it would be
lying. The caller relationship is still there — pass `includeStacks: true` to
`cbox_telemetry_finish()` and you get the call tree as well.

## Only keep it when it matters

Profiling can run for every sampled unit of work; the cost of *keeping* a
profile is only paid when you ask for one:

```php
$result = cbox_telemetry_finish($handle, includeProfile: $durationMs >= 500);
```

A fast request resets the native state and allocates nothing into PHP. A slow
one materialises the profile. The extension does not make that call — it has no
idea what "slow" means for your application.

## Next

- [How the profiler works](core-concepts/profiler.md)
- [Configuration reference](configuration/reference.md)
