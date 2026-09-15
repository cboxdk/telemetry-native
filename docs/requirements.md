---
title: "Requirements"
weight: 3
description: "PHP versions, platforms and build tooling this extension actually supports."
---

# Requirements

Everything here is enforced by `composer.json` or by `configure`. Nothing is
aspirational.

## PHP

`^8.3 | ^8.4 | ^8.5`, matching `cboxdk/laravel-telemetry` so the extension
installs anywhere that package does.

Non-thread-safe builds only. `composer.json` declares `support-zts: false`, so
PIE will refuse a ZTS build rather than produce one that misbehaves. The code is
written TSRM-clean and two clearly-marked places would need work first — the
sampler's tick counter and the crash handler's state, both of which are
deliberately file-static because resolving a TSRM cache from a signal handler is
not async-signal-safe.

## Platforms

| | status |
|---|---|
| Linux x86_64 / arm64, glibc | supported — CPU-time sampling via `timer_create` |
| Linux x86_64 / arm64, musl (Alpine) | supported, same backend |
| macOS arm64 | development only — everything except CPU profiling, which is off by default there ([why](KNOWN-ISSUES.md)) |
| Windows | out of scope; `composer.json` excludes it |

The difference between Linux and macOS is not cosmetic. See
[decision 0002](decisions/0002-timer-backends.md).

## Build tooling

Source builds need the usual PHP extension toolchain: `phpize`, `autoconf`,
a C compiler and `make`. PIE will offer to install them for you on Debian,
Ubuntu, Fedora, Enterprise Linux, Amazon Linux, Arch, Alpine and Homebrew.

No third-party libraries. The extension links against libc, pthreads and — on
Linux — librt, all of which are already in any PHP build.

## Optional at runtime

Nothing is required. Operation hooks attach to whatever is present:
`ext-pdo` for connection timing, `ext-redis` for Redis connects, `ext-curl` for
transfers. A missing extension means that hook simply never installs; it is not
an error and not a warning.
