# Security

## Reporting

Please do not open a public issue for a security problem. Email sn@cbox.dk, or
use GitHub's private vulnerability reporting on this repository. Best effort, no
promised response time.

## What this extension collects

By design, the minimum that answers "where did the time go" and "what was it
doing when it died":

- Function names, class names, declaring file paths and declaration lines, as
  part of profiling.
- Durations and counts of a fixed set of native operations, by type.
- Engine counters (GC runs, sample counts, buffer overflows).
- Fixed-width breadcrumbs whose labels are operation and unit names only.

It does **not** capture function arguments, request bodies, query parameters,
SQL text or bindings, environment variables, connection strings, cookies,
headers, or any other application value — not by default and not behind a flag.
There is no code path that reads them.

Crash records carry the same bounded data plus a signal number and `si_code`, a
pid, a timestamp, the trace/span ids the caller supplied, and — for diagnosing
crashes — three raw memory addresses: the faulting address, the program counter,
and where this extension is mapped. Those are machine addresses, not application
data, but they do describe the process's memory layout, so treat a crash record
as something to keep inside your own infrastructure. Breadcrumb labels are
truncated to 48 bytes at write time and are never free-form application strings.

Application-level redaction is `cboxdk/laravel-telemetry`'s job, and it remains
so. This extension has nothing to redact.

## The crash sink

Crash records are written to `cbox_telemetry.crash.dir` (default
`/tmp/cbox-telemetry`). The directory is created `0700` and the file `0600`; it
is opened `O_NOFOLLOW`, and a pre-existing directory is refused unless it is a
real directory owned by the current user. If any of that fails the recorder
disables itself and reports why in `cbox_telemetry_status()` — it never falls
back to somewhere less safe.

## Signal handlers

The extension installs handlers for `SIGSEGV`, `SIGABRT`, `SIGBUS` and `SIGILL`.
They chain to whatever was installed before and let the process die exactly as
it would have, core dump included. `cbox_telemetry.crash.enabled=0` disables
them entirely.
