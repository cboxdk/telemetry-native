---
title: "Crash records"
weight: 24
description: "What the extension can tell you about a segfault, and what it deliberately cannot."
---

# Crash records

When PHP dies below the level its exception handler can see — a segfault in an
extension, an abort in a native library, a bus error — everything in userland
disappears with it. There is no shutdown function, no log line, no trace.

The crash recorder exists to leave one small, honest artefact behind.

## What a record contains

```php
[
    'signal'       => 6,
    'signal_name'  => 'SIGABRT',
    'si_code'      => 0,
    'pid'          => 4711,
    'timestamp_ns' => 1789...,
    'unit'         => 'queue',
    'unit_duration_ns' => 812_000_000,
    'trace_id'     => '4bf92f3577b34da6a3ce929d0e0e4736',
    'span_id'      => '00f067aa0ba902b7',
    'operation'    => 'pdo.connect',
    'operation_elapsed_ns' => 180_000_000,
    'breadcrumbs'  => [
        ['seq' => 1, 'type' => 'unit.begin', 'label' => 'queue',       'ts_ns' => …],
        ['seq' => 2, 'type' => 'op.begin',   'label' => 'pdo.connect', 'ts_ns' => …],
    ],
]
```

The trace and span ids are the ones the caller passed to `begin()`, which means
a crash can be correlated with the trace that was in flight when it happened.

## Breadcrumbs

A fixed ring of fixed-width entries — 64 bytes each, `breadcrumbs.size` of them,
allocated once. Unit boundaries and operation boundaries, nothing else.

The labels are **copied at write time**, truncated to 48 bytes. That copy is the
whole design: after a segfault there is no frame table left to resolve an id
against, so anything a record needs in order to be readable has to already be
inside it.

Breadcrumbs outlive units. A crash outside any unit of work still gets whatever
context there is.

## Safety

The handler runs in signal context after something has already gone badly wrong,
so it does only what POSIX guarantees is safe there:

- fills in the mutable fields of a record that was formatted at startup
- opens its own sink and `write()`s to it — `open` and `write` are both
  async-signal-safe, and the path was built beforehand because `snprintf` is not
- restores the previous handler and gets out of the way

No allocation. No Zend API. No PHP callbacks. No locks. No stdio. A latch makes
a crash inside the crash handler `_exit` rather than loop.

Each record is written with a single `O_APPEND` write to that process's own
sink, so there is nothing to interleave with.

## The process still dies normally

The handler restores whatever disposition was there before and lets the process
die exactly as it would have — core dump, FPM child accounting, everything. A
handler that swallowed the signal would be far worse than no handler.

The handlers are installed with `SA_NODEFER`, so the signal stays deliverable
while we are handling it. Without that, a fault *inside* the handler is masked,
and Darwin neither force-delivers it nor re-enters the handler: the thread
re-executes the faulting instruction forever. The re-entrancy latch turns that
into a clean `_exit`, but only if the signal can be delivered at all.

It always re-raises after restoring. An earlier version only did that for
signals it judged to have been *sent*, trusting a hardware fault to re-trap on
return — but `si_code` does not mean the same thing everywhere. Darwin reports
`si_code = 2` for a `kill()`-sent SIGSEGV too, so that version classified it as
a fault, returned, and left the process running after a fatal signal, having
already written a crash record for it. There is a second cost, measured rather than assumed: under a storm of three
or more identical fatal signals within a few milliseconds, a later one can
re-enter the handler while the first is still writing. The re-entrancy latch
turns that into a clean `_exit(128 + signal)`, so the process still dies
correctly — but that record is lost. In a 40-signal burst roughly half produced
no record. A single fatal signal, which is the real case, was recorded every
time on every platform tested.

There is a real cost to that, and it is worth stating: because the signal is
unblocked, `raise()` terminates the process from inside the handler rather than
letting the original instruction re-trap. A core dump therefore records the
`raise` as the terminating context rather than the faulting instruction, which
makes core-based grouping less precise. The record written just before it
carries the faulting address and program counter, so the information is not
lost — but a process that survives a fatal signal is a worse failure than a
slightly less precise core, which is why it is done this way.

If another handler was installed before ours, it is chained rather than
replaced.

## One sink per process

Each process writes its own file, `crash-<pid>.bin`. The path is reserved on
the process's first request; the file itself is created by the handler, if
there is ever anything to put in it. Three things follow from that.

**Per process, because draining a shared file is not safe.** Compacting one
file means truncating it, and anything appended between the last read and the
truncate is gone. The handler cannot take a lock to close that window — locking
is not async-signal-safe — so the window cannot be closed, only avoided. A
drain therefore only touches files whose owning process is gone: a dead process
appends nothing, so its file can be read whole and removed. Live processes'
sinks are skipped entirely, because unlinking one would leave its owner writing
into a file with no name and lose the very crash it was there to catch.

**In a directory only this user can open.** The configured `crash.dir` is a
*base*; each uid gets its own `0700` subdirectory under it, named for the uid,
and sinks live there. The mode is forced with `chmod` after `mkdir`, because
`mkdir`'s mode argument is masked by the umask — under the usual `022` a
directory asked for as `01733` is created `01711`.

A base owned by another user is refused outright unless that user is root and
the directory follows the `/tmp` contract (sticky and world-writable). Sticky
alone is not enough: it stops *other users* removing an entry, but the
directory's owner can always remove entries in it, so an unprivileged user who
wins the race to create the base could otherwise delete or rename every crash
record on the host. Losing that race now costs crash recording — reported as
`unavailable: directory` — rather than handing it to whoever won.

For several FPM pools running as different users, point them at one
root-provisioned base (`mkdir -m 01777`, root-owned) or give each pool its own
`crash.dir`.

**Prepared on the first request, because of PHP-FPM.** The master starts as
root and runs module startup; workers then drop to another user. A directory
created at startup belongs to root with mode 0700, and every worker is locked
out of it for the life of the pool — the recorder reports
`unavailable: directory` and records nothing. Preparing on the first request
means it belongs to whoever actually writes it. `tests/fpm/run.sh` exercises
exactly this against a real FPM master with a non-root pool.

**Created only on a crash, because empty files are litter.** Creating the sink
up front leaves a zero-byte file behind for every process that never crashes —
one per worker lifetime, which on a recycling FPM pool is thousands a day in a
shared directory. `open()` and `write()` are both async-signal-safe, so the
handler can create the file itself; the path is built beforehand precisely
because `snprintf` is not.

The consequence for multi-pool setups: the first pool to serve a request owns
the directory, and pools running as a different user will report
`unavailable: directory`. Give each pool its own `cbox_telemetry.crash.dir`.

## Consistency of a crash snapshot

A crash lands in the middle of whatever the process was doing, including
writing a breadcrumb. Entries are therefore published with their sequence
number written last: a slot being written reads as sequence zero and is
skipped, and an entry whose sequence changes while it is being copied is
dropped. A record contains only breadcrumbs that were complete.

There is no retry loop. This runs in a signal handler after something has
already gone wrong, and spinning there would be a worse failure than one
missing breadcrumb.

## Draining

Records outlive the process that wrote them. Another process reads them later:

```php
foreach (cbox_telemetry_drain_crashes() as $record) {
    // hand to whatever exports telemetry
}
```

Draining consumes: records handed over are removed, and anything appended while
the drain was reading is preserved. The extension does not export crash records
itself — it has no network access and no opinion about where they go.

## What it is not

Not a debugger and not a core-dump processor. There is no native stack unwinding
and no symbolication: a record tells you *what PHP was doing* before it died,
not which C frame died. For the C side, use the core dump — which still exists,
precisely because this handler gets out of the way.

`systemd-coredump`, Crashpad and friends remain the right tools for native
stacks. This complements them by adding the PHP-level context they cannot see.
