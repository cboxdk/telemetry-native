# Known issues

## Sampling is unsafe on platforms without a per-thread CPU timer

**Status:** diagnosed, and disabled by default on the affected platforms.
Linux — the production target — is unaffected.

### What happens

On a platform with no `timer_create(CLOCK_THREAD_CPUTIME_ID)` (in practice
macOS), the profiler falls back to a sampler thread. Interrupting the VM from
that thread corrupts it: processes die with `SIGSEGV`, in a different place each
time.

How often depends heavily on the workload, and an earlier version of this
document understated it by quoting only the test suite. Measured since: 7 of 400
short profiled processes at a 100 µs period; 2 of 3 runs of a fixture that
defines several hundred functions and recurses 120 deep. "One in a few hundred"
was true of one workload, not of the platform.

The extension's own crash recorder caught one with the fault address and the
faulting instruction:

```
signal=SIGSEGV  fault_address=0x18
program_counter=0x104816098   module_base=0x106960000   module_offset=NULL
```

`0x18` is the `func` field of `zend_execute_data`, so a NULL frame is being
dereferenced — and the program counter sits *below* this extension's mapping,
so the faulting instruction belongs to the PHP binary, not to us. That matches
a backtrace taken separately: `EX(call)` observed NULL inside `ZEND_DO_UCALL`,
which is the VM taking its interrupt path at a moment when its own local frame
and `EG(current_execute_data)` disagree.

### How it was isolated

Crash records counted over 20 full suite runs per arm — records, not test
failures, so an assertion flake cannot be mistaken for a crash:

| | crash records |
|---|---|
| profiler running (sampler thread sets the VM interrupt flag) | 2 / 20 |
| profiler disabled | 0 / 20 |
| profiler running, only the cross-thread interrupt store removed | 0 / 20 |

One change, one variable, and the crash follows it.

A second approach made it worse rather than better: having the sampler thread
`pthread_kill()` the PHP thread, so the interrupt flag is set by a handler
running *on* that thread — which is exactly what the Linux backend does —
produced **13 crash records over 40 runs**. The reason is the difference the
Linux backend gets for free: a per-thread CPU-time timer only fires while that
thread is executing PHP, whereas `pthread_kill` can land it anywhere, including
mid-syscall, inside the allocator, or on a Fiber's stack.

### Why Linux appears unaffected

Linux uses `timer_create(CLOCK_THREAD_CPUTIME_ID)` with `SIGEV_THREAD_ID`: the
kernel delivers the signal to the PHP thread itself, and the handler sets the
interrupt flag from that same thread. Nothing is cross-thread.

An earlier version of this document went further and claimed the timer "only
fires while that thread is executing PHP". That is wrong, and the correction
matters because it was load-bearing in the argument: `CLOCK_THREAD_CPUTIME_ID`
counts system time as well as user time, and the signal lands on an arbitrary
instruction — inside the allocator, on a Fiber's C stack, in the kernel-entry
path of a syscall. Nothing about it is restricted to VM-safe points.

So the honest statement is the empirical one below, not a mechanism. Why the
cross-thread store is so much worse in practice is not established.

Zero occurrences across every Linux run to date: repeated local suite runs on
Debian glibc and Alpine musl, a Valgrind-clean run, the FPM privilege-separation
test, and CI across x86_64, arm64, glibc and musl.

### What the extension does about it

Profiling is **off by default** wherever the timer backend is not a per-thread
CPU timer. `cbox_telemetry_status()` says so rather than silently collecting
nothing:

```
profiler_enabled => false
profiler_status  => disabled: no per-thread CPU timer on this platform, and
                    sampling through the fallback backend can corrupt the VM
                    (cbox_telemetry.profiler.allow_fallback_backend=1 to override)
```

Nothing else is affected: native operation timing, runtime counters,
breadcrumbs and the crash recorder all work normally there.

To sample anyway, knowing the above:

```ini
cbox_telemetry.profiler.allow_fallback_backend = 1
```

This repo's own test suite sets exactly that, because its job is to exercise the
profiler. That is why the macOS CI jobs are marked `continue-on-error`: they
deliberately run the unsafe path, and a rare crash there is expected rather than
a regression. A default installation never takes that path.

### Not closed

Disabling something is not repairing it. What would repair it is a way to
interrupt the VM on Darwin that is as narrow as the Linux timer — something
that fires only while the thread is executing PHP. `kqueue`'s `EVFILT_TIMER`
has the same cross-thread problem; Mach thread-suspend plus an inspection of
the suspended thread might not, but that is a substantially larger piece of
work than the platform justifies today.

Since macOS is a development platform for this extension and Linux is the only
supported production target, that work has not been done.
