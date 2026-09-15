---
title: "Cbox Telemetry Native"
weight: 1
description: "An optional PHP extension that measures the runtime: CPU profiling, native operation timing, runtime counters and crash records."
---

# Cbox Telemetry Native

`cbox_telemetry` is an optional PHP extension that observes and measures the PHP
runtime. It is the depth layer under [`cboxdk/laravel-telemetry`][lt] — and
strictly a layer, not a replacement.

## The mental model

Three things, with a hard line between them:

| | owns |
|---|---|
| `laravel-telemetry` | trace context, sampling policy, attribute naming, redaction, OTLP export, everything Laravel-shaped |
| `cboxdk/system-metrics` | host, process and cgroup metrics |
| **this extension** | the PHP runtime itself — call stacks, connection timing, engine counters, crash context |

The extension has no opinion about what is interesting. It measures what it is
told to measure, hands back aggregates, and forgets. The caller decides whether
a unit of work was worth keeping, what to call it, and where it goes.

That boundary is what keeps the C small: no Laravel knowledge, no exporters, no
sockets, no policy.

## What it can answer that PHP alone cannot

- Which call stacks actually burned the CPU during a slow request.
- How long establishing that database connection took, as opposed to querying.
- How long `curl_exec` spent, including the calls Laravel's HTTP client never
  sees.
- What the process was doing in the moments before it segfaulted.

## Sections

- [Getting started](getting-started/_index.md) — install it, check it loaded, test against it.
- [Core concepts](core-concepts/_index.md) — how the profiler, the operation hooks and the crash recorder work.
- [Configuration](configuration/_index.md) — every INI setting and its bounds.
- [Cookbook](cookbook/_index.md) — wiring it to a unit of work.
- [Security](security/_index.md) — exactly what is and is not collected.
- [Decisions](decisions/_index.md) — the two that testing forced.

[lt]: https://github.com/cboxdk/laravel-telemetry
