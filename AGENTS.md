# Agent guide — cboxdk/telemetry-native

A PHP extension written in C. The first one in the org, so there is no sibling
extension to copy from — the *repo* conventions come from
`cboxdk/laravel-telemetry`, the C has to stand on its own.

## Commands

```bash
phpize && ./configure --enable-cbox-telemetry && make -j8
make test                      # the .phpt suite
make clean && make -j8         # after ANY change to a function signature
php benchmarks/run.php modules/cbox_telemetry.so 21 1000
php benchmarks/profile_accuracy.php
```

`make clean` is not optional after signature changes. An incremental build will
happily link a stale object and produce a `.so` that the loader kills with
SIGKILL and no error message.

On macOS, build against Homebrew PHP — Herd ships no headers:

```bash
./configure --enable-cbox-telemetry --with-php-config=/opt/homebrew/bin/php-config
```

Linux is the target that matters and cannot be tested natively on a Mac. Use
Docker for anything you intend to believe:

```bash
docker run --rm -v "$PWD":/src:ro php:8.4-cli bash -c '…'   # see .github/workflows/ci.yml
```

## Architecture map

```
cbox_telemetry.c      module entry, INI, the five PHP functions, profile → zval
php_cbox_telemetry.h  module globals
src/profiler.c        the sampler: signal-context tick, safe-point stack walk
src/timer_posix.c     Linux: timer_create(CLOCK_THREAD_CPUTIME_ID) + SIGRTMIN+4
src/timer_thread.c    everything else: timer thread, no signals, wall clock
src/arena.c           bump allocator, O(1) reset
src/frames.c          frame interning (names copied, never zend_string*)
src/stacktree.c       call-path trie
src/ops.c             per-type operation aggregates + nesting stack
src/hooks.c           targeted internal-handler swaps
src/breadcrumbs.c     fixed-width crash ring
src/crash.c           sigaction, record encode/decode, drain
src/sigstack.c        one alternate signal stack, shared
```

## Invariants — do not break these

1. **The signal handler does two things.** A relaxed atomic add and an atomic
   store to `EG(vm_interrupt)`. Nothing else is async-signal-safe, so nothing
   else goes in there. The stack walk happens at the VM's safe point.
2. **No Zend allocation per sample.** Names go into the arena, paths into the
   trie. If you find yourself reaching for `emalloc` on the sampling path, the
   design is wrong. `emalloc` is fine in `finish()`, which runs in PHP context.
3. **Never `SIGPROF`, never `ITIMER_PROF`.** They are PHP's `max_execution_time`
   on builds without `ZEND_MAX_EXECUTION_TIMERS`. See decision 0002.
4. **Everything is bounded and fails open.** Frames, nodes, breadcrumbs, arena,
   operation nesting. Past the ceiling: increment a dropped counter and carry
   on. Never grow, never block, never fail the request.
5. **The crash handler allocates nothing and calls no PHP.** It fills a
   preformatted record, `write()`s it, restores the previous disposition and
   returns — re-raising only for signals that were *sent* rather than trapped,
   because those do not re-trap on return.
6. **No Laravel knowledge in C.** Not routes, not job classes, not config. A
   unit of work is a label and a trace id.
7. **No networking from C.** Ever. Export is `laravel-telemetry`'s job.
8. **Never copy from Excimer.** It is GPL-2.0 and this is MIT. Its `config.m4`
   is readable as evidence of which POSIX APIs exist where; the implementation
   is ours.
9. **Signature changes need `make clean`.** See above.

## Conventions

- Tabs for indentation, K&R braces, `cbox_` prefix on every non-static symbol.
- One `.c`/`.h` pair per concern; headers carry the *why*, not a restatement of
  the signature.
- Every new source file goes in `config.m4`'s `PHP_NEW_EXTENSION` list — and
  that list must not end with a newline before the closing bracket, or the
  generated `configure` dies with a bare `; do`.
- New PHP-visible functions: edit `cbox_telemetry.stub.php`, then regenerate
  with php-src's `build/gen_stub.php`. Never hand-edit `_arginfo.h`.
- Tests are `.phpt`. Anything that intentionally crashes runs in an isolated
  child process, never in the test process.
- Claims about overhead come from `benchmarks/`, not from intuition. The
  containerised noise floor is ~±4%; say so rather than quoting a number it
  cannot support.
- **No `bin/check-licenses.php` or `bin/generate-sbom.php` here, deliberately.**
  Sibling packages ship both because they have a `composer.lock` to inspect.
  This one has zero Composer dependencies and links only against libc, pthreads
  and librt, so those scripts would have nothing to report. Add them the moment
  a dependency appears.
