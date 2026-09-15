---
title: "Security and privacy"
weight: 50
description: "What the extension collects, what it refuses to collect, and how the crash sink is protected."
---

# Security and privacy

The short version: this extension collects names and numbers, never values.

## Collected

- Function names, class names, declaring file paths and declaration lines.
- Durations and counts of a fixed set of native operations, by type.
- Engine counters — GC runs and collections, sample and drop counts, buffer
  overflow counts, arena high-water mark.
- Breadcrumbs whose labels are operation and unit type names, truncated to
  48 bytes at write time.
- In crash records: a signal number and `si_code`, a pid, a timestamp, the
  trace and span ids the caller supplied, and — for diagnosing the crash itself
  — three raw memory addresses: the faulting address, the program counter, and
  where this extension is mapped. Those are machine addresses rather than
  application data, but they do describe the process's memory layout, so keep
  crash records inside your own infrastructure.

## Not collected

Function arguments. Request bodies. Query parameters. SQL text or bindings.
Environment variables. Connection strings. Cookies. Headers. Any other
application value.

Not by default, and not behind a flag — there is no code path that reads them.
An extension that *could* capture arguments would need a redaction story, and
the right way to not need one is to not capture them.

File paths and function names do reach the consumer as part of profiling. They
are the profile; a profiler that hid them would have nothing to say.

## Redaction

None here. Application-level redaction belongs to
[`cboxdk/laravel-telemetry`][lt], which has the context to know what a value
means. This extension has nothing to redact.

## The crash sink

`cbox_telemetry.crash.dir` defaults to `/tmp/cbox-telemetry`, which is a shared
location, so:

- The directory is created `0700` and the file `0600`.
- A pre-existing path is used only if it is a real directory (not a symlink) and
  owned by the current user. Anything else disarms the recorder.
- The sink is opened `O_NOFOLLOW`.
- If any of that fails, the recorder disables itself and reports the reason in
  `cbox_telemetry_status()['crash_recorder']`. It never falls back to a location
  it cannot vouch for.

Records are fixed-width and bounded. Nothing of variable length, and nothing
originating in application data, is ever written into one.

## Signal handlers

Handlers are installed for `SIGSEGV`, `SIGABRT`, `SIGBUS` and `SIGILL`. They
chain to whatever was installed before, allocate nothing, call no PHP, and let
the process die exactly as it would have — core dump included.

`cbox_telemetry.crash.enabled=0` removes them entirely. That switch exists
because a signal handler in every PHP process on a machine deserves an
unambiguous off.

## Honest scope

This is a measurement tool, not a security control. It does not sandbox, does
not validate, and does not defend against a hostile application — it runs
in-process with whatever it is measuring. Treat its output as data produced by
that application.

[lt]: https://github.com/cboxdk/laravel-telemetry
