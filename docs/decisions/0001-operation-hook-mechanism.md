---
title: "0001 — Hook internal handlers, not the Zend Observer"
weight: 1
description: "Why native operation timing swaps six function handlers instead of registering a Zend Observer."
---

# 0001 — Hook internal handlers, not the Zend Observer

Status: accepted, 2026-09-15.

## Context

The extension needs exact begin/end timing for a handful of calls that Laravel
cannot see the inside of: `PDO::__construct`, `Redis::connect`, `Redis::pconnect`,
`curl_exec`, and optionally `stream_socket_client` / `fsockopen`.

Two mechanisms were on the table:

1. **Zend Observer.** `zend_observer_fcall_register()` in MINIT, returning a
   begin/end pair only for the functions we care about and declining everything
   else. This is the documented, forward-looking API and was the PRD's stated
   preference.
2. **Targeted handler swap.** Look each function up once and replace its
   `internal_function.handler` with a wrapper that brackets the original.

The PRD asked for this to be settled by benchmark rather than preference.

## Decision

Swap the handlers.

## Why

Declining a function in the observer's registration callback does not make that
function free. Registering *any* observer switches the engine onto its
observed-call path for **every** call in the process, and that shows up
immediately on call-heavy PHP.

Median wall time, PHP 8.4 on Linux, 7 iterations per cell, relative to the same
build with the extension absent:

| mode | cpu | calls | deep | internal | mixed |
|---|---|---|---|---|---|
| extension loaded, no hooks | -1.1% | -0.1% | -2.4% | -0.3% | 2.0% |
| **observer registered** | **23.7%** | **19.2%** | **19.0%** | **9.9%** | **6.7%** |
| handler swap | 0.2% | 0.6% | -0.2% | 2.1% | 0.0% |

The observer costs ~20% on workloads that never call a single observed function.
The handler swap is indistinguishable from not being there. For an extension
whose entire premise is being safe to leave on in production, that is not a
close call.

## Consequences

- We replace engine handlers, which the PRD's own principles say to avoid unless
  necessary. The benchmark is the necessity. The swap is narrow — six named
  functions, resolved once — not a global replacement.
- Hooks install at **RINIT**, not MINIT: `PDO` and `Redis` belong to other
  extensions whose MINIT may run after ours, so the classes do not exist yet
  when ours runs.
- We install only if the current handler is not already someone else's wrapper
  we would be cutting out, and on shutdown we restore only if the handler is
  still ours. Another APM that wraps us afterwards keeps working.
- A `zend_bailout` (fatal error, timeout, `exit()`) longjmps past the end of our
  wrapper, so an operation that dies mid-call never records its end. That costs
  one measurement, never correctness: the nesting stack is bounded and is reset
  at every unit boundary and at RSHUTDOWN. Wrapping each call in `zend_try`
  would close the gap at the price of a `setjmp` per `curl_exec`, which is a
  worse trade than losing the timing of a call that killed the request anyway.
- If a future PHP makes observers free for non-observed functions, this decision
  should be re-measured, not assumed to still hold.
