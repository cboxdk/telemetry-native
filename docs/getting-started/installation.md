---
title: "Installation"
weight: 11
description: "Installing via PIE or from source, and verifying the build you got."
---

# Installation

## PIE

```bash
pie install cboxdk/telemetry-native
```

PIE compiles from source on Linux and macOS. It will offer to install the build
toolchain if it is missing.

## From source

```bash
git clone https://github.com/cboxdk/telemetry-native
cd telemetry-native
phpize
./configure --enable-cbox-telemetry
make -j8
make test
sudo make install
```

Then add `extension=cbox_telemetry.so` to your `php.ini`.

On macOS, build against Homebrew PHP — Herd ships no development headers:

```bash
./configure --enable-cbox-telemetry --with-php-config=/opt/homebrew/bin/php-config
```

## Verify

```bash
php -r 'print_r(cbox_telemetry_status());'
```

```php
[version] => 0.1.0
[enabled] => 1
[timer_backend] => posix-thread-cputime
[timer_cpu_time] => 1
[timer_signal] => 38
[profiler_status] => ready
[hooks] => pdo,redis,curl
[hooks_installed] => Array([0] => PDO::__construct, [1] => PDO::connect, [2] => curl_exec)
[hook_detail] => Array(
    [pdo]   => Array([requested] => 1, [installed] => 2, [unavailable] => 0, [active] => 1)
    [redis] => Array([requested] => 1, [installed] => 0, [unavailable] => 2, [active] => )
    …
)
[crash_recorder] => armed
[crash_path] => /tmp/cbox-telemetry/crash-4711.bin
```

What to look for:

- **`timer_backend`** — `posix-thread-cputime` is the real one. `thread-walltime`
  means you are on the development fallback and samples measure wall time.
- **`timer_signal`** — never 27. That is `SIGPROF`, which belongs to PHP's
  `max_execution_time`, and the extension deliberately stays off it.
- **`crash_recorder`** — `armed`, or a reason it is not: `unavailable: directory`
  (the crash directory is missing, not a directory, or not owned by this user),
  `unavailable: sink`, `unavailable: handler`, or `off` if you disabled it.
- **`hook_detail`** — the one to read. `requested` is configuration; `installed`
  is how many functions were actually wrapped. The example above says Redis
  hooks were asked for and found nothing, which is the answer to "why are there
  no `redis.connect` timings" — `ext-redis` is not installed. `hooks` alone
  cannot tell you that, because it only echoes the INI.
- **`profiler_status`** — `ready`, `disabled by configuration`, or why it is
  degraded (`unavailable: timer could not be created`). `profiler_enabled=false`
  on its own does not say whether that was a choice or a failure.

## Troubleshooting

**The extension does not appear in `php -m` and PHP exits without a message.**
On macOS a `.so` with an unresolved symbol is killed by the loader with no
diagnostic. Almost always a stale object file: `make clean && make`.

**`Maximum execution time of 0 seconds exceeded` on every profiled script.**
You are running a build old enough to have used `SIGPROF`. Update; nothing
current does.

**`crash_recorder` says `unavailable: directory`.** Something already owns
`cbox_telemetry.crash.dir` — commonly another user's `/tmp/cbox-telemetry` from a
different account. Point `cbox_telemetry.crash.dir` somewhere this user owns.
The recorder refuses rather than writing into a directory it cannot vouch for.
