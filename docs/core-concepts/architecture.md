---
title: "Architecture"
weight: 21
description: "The unit of work, the five functions, and the rules the C side holds itself to."
---

# Architecture

## The unit of work

Everything is scoped to a unit of work: an HTTP request, a queue job, an Artisan
command, a scheduled task, or anything else the caller wants to bracket.

```
begin(context)  →  handle
    │
    ├── the timer samples, if profiling was asked for and the unit is sampled
    ├── hooked operations accumulate into per-type aggregates
    └── breadcrumbs accumulate in a ring that outlives the unit
    │
finish(handle, includeProfile)  →  aggregates, and a profile only if asked
```

Units do not nest. A second `begin()` abandons the first rather than stacking —
a caller that leaks a handle degrades to "last one wins" instead of leaving a
timer armed forever. `finish()` resets everything immediately rather than at the
next `begin()`, so a worker waiting for its next job is not holding the previous
job's profile.

## The five functions

The PHP surface is deliberately tiny. Every boundary crossing costs something,
and a wide API invites callers to build policy on top of the wrong layer.

| | |
|---|---|
| `cbox_telemetry_version()` | the extension version |
| `cbox_telemetry_status()` | build mode, timer backend, active hooks, crash state, bounds |
| `cbox_telemetry_begin($context)` | start a unit; returns a handle, or `0` for "not started" |
| `cbox_telemetry_finish($handle, $includeProfile, $includeStacks)` | end it, return aggregates |
| `cbox_telemetry_drain_crashes($max)` | read and consume records left by processes that died |

`drain_crashes()` exists so the binary crash format stays an implementation
detail. Decoding it in PHP with `unpack()` would freeze the layout the moment
anyone shipped a parser.

## Rules the C side holds itself to

**No Laravel knowledge.** A unit is a label from a fixed set and an optional
trace id. There are no routes, no job classes, no config. If a name would need
to know what Laravel is, it belongs in `laravel-telemetry`.

**No networking.** The extension never opens a socket. Export is somebody
else's job, and keeping it that way is what makes the extension safe to leave
loaded.

**No policy.** The extension does not decide whether a request was slow, whether
a trace is sampled, or whether a profile is worth keeping. It is told.

**Everything bounded.** Frames, call-tree nodes, breadcrumbs, the arena and the
operation nesting stack all have hard ceilings, and configuration is clamped to
them rather than trusted. Past a ceiling the extension increments a dropped
counter and carries on: it never grows, never blocks, never fails the request.

**Fail open, everywhere.** If the timer cannot be created, profiling turns
itself off and the rest keeps working. If the crash directory is unusable, the
recorder disarms and says so in `status()`. Telemetry that takes down the
application it is measuring is worse than no telemetry.

## Memory

One arena per process, sized from `max_frames` — about 1 MB at the defaults,
allocated once at startup and reset with a pointer store between units. Plus the
fixed frame table, the fixed trie and the breadcrumb ring. Nothing is allocated
per sample, per operation or per unit, so a worker that runs ten thousand jobs
uses the same memory as one that runs one.
