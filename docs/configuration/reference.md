---
title: "Configuration reference"
weight: 31
description: "Every cbox_telemetry.* INI setting, with defaults, bounds and mutability."
---

# Configuration reference

Every value is clamped to the bounds below rather than trusted. An out-of-range
setting is silently corrected, never an error — `cbox_telemetry_status()['limits']`
reports what is actually in force.

## Why so much of this is `PHP_INI_SYSTEM`

Anything that decides what gets installed at startup cannot be changed per
request: signal handlers, the crash sink, the size of preallocated buffers. Those
are `PHP_INI_SYSTEM`. The things a caller genuinely needs to vary — the sampling
period and stack depth — are `PHP_INI_ALL`, *and* can be overridden per unit of
work through the `begin()` context, which is the better place to do it.

## Settings

| setting | default | bounds | changeable | |
|---|---|---|---|---|
| `cbox_telemetry.enabled` | `1` | | SYSTEM | Master switch. Off means MINIT returns immediately: no timer, no hooks, no handlers. |
| `cbox_telemetry.profiler.enabled` | `1` | | SYSTEM | Off means no timer is created and `begin()` never profiles. |
| `cbox_telemetry.profiler.period_us` | `1000` | 100 – 100000 | ALL | Sampling period in microseconds. Overridable per unit via `period_us`. |
| `cbox_telemetry.profiler.max_depth` | `64` | 1 – 256 | ALL | Frames walked per sample. Deeper stacks get a `<truncated>` marker. Overridable per unit via `max_depth`. |
| `cbox_telemetry.profiler.max_frames` | `4096` | 64 – 65536 | SYSTEM | Distinct functions per unit. Also sizes the arena (≈ 256 bytes each). |
| `cbox_telemetry.profiler.max_nodes` | `16384` | 256 – 262144 | SYSTEM | Distinct call paths per unit. |
| `cbox_telemetry.hooks.pdo` | `1` | | SYSTEM | Time `PDO::__construct` and `PDO::connect`. |
| `cbox_telemetry.hooks.redis` | `1` | | SYSTEM | Time `Redis::connect` and `Redis::pconnect`. |
| `cbox_telemetry.hooks.curl` | `1` | | SYSTEM | Time `curl_exec`. |
| `cbox_telemetry.hooks.streams` | `0` | | SYSTEM | Time `stream_socket_client` and `fsockopen`. Off by default — noisy, and it overlaps whatever the caller already instruments. |
| `cbox_telemetry.breadcrumbs.size` | `256` | 16 – 4096 | SYSTEM | Ring entries, 64 bytes each. Rounded up to a power of two. |
| `cbox_telemetry.crash.enabled` | `1` | | SYSTEM | Install the fatal-signal handlers. The kill switch if they are ever suspected. |
| `cbox_telemetry.crash.dir` | `/tmp/cbox-telemetry` | | SYSTEM | Where records are written. Created `0700`; refused if it exists and is not a directory owned by this user. |
| `cbox_telemetry.auto` | `0` | | SYSTEM | Open a unit of work automatically at the start of every request. See below. |
| `cbox_telemetry.auto_max_ms` | `60000` | 1000 – 3600000 | SYSTEM | How long an automatic unit may sample before the profiler stops itself. Only applies to automatic units. |

## Automatic instrumentation

With `cbox_telemetry.auto=1` a unit of work opens at RINIT, before any PHP runs.
Two things follow from that.

**You can collect without ever calling `begin()`.** `cbox_telemetry_finish(0)`
ends whichever unit is open, which is all a terminate hook has to work with.

**Measurement starts at the first instruction.** A `begin()` in framework
middleware cannot see autoloading, service providers, config or route caching —
on a cold request that is often most of the time. An automatic unit does. The
first `begin()` then *adopts* the running unit rather than restarting it: the
bootstrap samples are kept and the context passed in labels them.

It is off by default because it is wrong for long-running processes. In a queue
worker or an Octane server, RINIT fires once for the whole process, so an
automatic unit would cover hours and mean nothing. Those should call
`begin()`/`finish()` per job, with `auto=0`.

`auto_max_ms` is the safety valve for when that advice is not followed: an
automatic unit that outlives it stops sampling, keeps what it has, and sets
`counters['profiler.capped']`. It bounds the damage; it is not a substitute for
turning `auto` off where it does not belong.

## Per-unit overrides

`cbox_telemetry_begin()` accepts a context array. These are the only keys it
reads; anything else is ignored.

| key | type | |
|---|---|---|
| `trace_id` | 32 hex chars | Stored raw for the crash record. An all-zero id counts as absent. |
| `span_id` | 16 hex chars | Same. |
| `unit` | `http`\|`queue`\|`command`\|`schedule` | Anything else becomes `other`. |
| `sampled` | bool | `false` means the profiler never starts, whatever else you asked for. |
| `profile` | bool | Whether to run the profiler for this unit at all. |
| `period_us` | int | Clamped to the INI bounds. |
| `max_depth` | int | Clamped to the INI bounds. |

## A note on layering

The INI decides what *exists*. The caller decides what *happens*. Those can
disagree — a consumer configured to want PDO timing on a build where
`cbox_telemetry.hooks.pdo=0` will simply never see `pdo.connect` aggregates, with
no error anywhere.

That is worth surfacing in whatever diagnostics the consumer offers.
`cbox_telemetry_status()` returns both `hook_detail` (what the INI enabled) and
`hooks` (the summary), which is enough to notice the mismatch and say so.

## PHP-FPM

The crash directory is created by the first worker to serve a request, not by
the master, so it belongs to the pool user rather than root. One consequence:
**give each pool its own `cbox_telemetry.crash.dir`** when pools run as
different users. The first pool to start owns the directory, and others will
report `unavailable: directory` in `cbox_telemetry_status()` — truthfully, but
they will record nothing.

## Sizing memory

The dominant cost is the arena, at roughly `max_frames × 256` bytes — about 1 MB
at the defaults, allocated once per process at startup, never grown. Plus the
frame table, the trie (`max_nodes × 12` bytes) and the breadcrumb ring
(`breadcrumbs.size × 64` bytes).

`cbox_telemetry_status()['arena_peak_bytes']` reports the high-water mark. If it
never approaches the ceiling, `max_frames` can come down.
