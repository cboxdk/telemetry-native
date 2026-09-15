# Known issues

## Rare SIGSEGV on macOS arm64 under the test harness

**Status:** open, unexplained. Affects the development platform only.

### What was measured

Running the full suite repeatedly on macOS arm64 (Apple Silicon, Homebrew
PHP 8.5), under the flags `make test` uses:

| | |
|---|---|
| suite runs, clean | 21 / 25 |
| suite runs where one test died of a signal | 3 / 25 |
| suite runs with a flaky assertion | 1 / 25 |

That is roughly three process deaths per five hundred test executions. A
different test dies each time — 003, 004, 006, 008, 011, 017, 019 have all been
seen — so it is not one bad test.

The extension's own crash recorder captured one: `SIGSEGV`, `si_code=2`
(`SEGV_ACCERR` — a protection fault, not a null dereference), a few hundred
microseconds into an ordinary unit of work, with breadcrumbs showing nothing
unusual before it.

### What it is not

Ruled out by measurement, not by argument:

- **Not the automatic-instrumentation work.** It reproduces at the same rate on
  the commit before that landed.
- **Not the crash recorder.** An early 25-run sample with the handlers disabled
  came back clean and pointed the finger at them; a larger sample did not.
  Three of the deaths above happened with the handlers off.
- **Not the operation hooks.** Disabling them does not change the rate.
- **Not one test, and not the JIT.** Most of the affected tests run with opcache
  inactive entirely.
- **Not reproducible standalone.** The same test, with the same flags, in a
  tight loop, 30 times: clean. It needs the whole suite.

### Not seen on Linux

Zero occurrences across 24+ local suite runs on Debian glibc and Alpine musl,
a Valgrind-clean run (0 errors, nothing held at exit), and 13 green CI jobs
spanning x86_64, arm64, glibc and musl.

That is reassuring rather than conclusive: the same defect could simply be
rarer there. Linux is the production target and the only platform where CPU-time
sampling is supported at all, so the practical exposure is low — but "we have
not seen it" is not "it is not there".

### Consequence for CI

macOS jobs are allowed to fail without failing the build, and are marked as
such in the workflow. Masking a known flake beats training everyone to ignore a
red X, but it is a stopgap, not a fix.

### Next step

Get a backtrace. The useful experiment is a hammered suite run with core dumps
enabled, or a temporary handler that records `si_addr` and the faulting context
alongside the existing crash record — `SEGV_ACCERR` plus a fault address should
identify which mapping is being touched, and that should name the culprit
quickly.
