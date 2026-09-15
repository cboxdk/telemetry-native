---
title: "Native operations"
weight: 23
description: "Exact begin/end timing for connection establishment and cURL, and why it is separate from sampling."
---

# Native operations

Sampling tells you where CPU went. It cannot tell you why opening a database
connection took 180 ms, because most of that time is not CPU at all — it is DNS,
a TCP handshake, TLS negotiation and authentication, none of which burn cycles
in PHP. A sampler sees almost nothing there.

So a small, fixed set of calls gets exact begin/end timestamps instead.

## What is hooked

| operation | target | enabled by default |
|---|---|---|
| `pdo.connect` | `PDO::__construct`, `PDO::connect` | yes |
| `redis.connect` | `Redis::connect` | yes |
| `redis.pconnect` | `Redis::pconnect` | yes |
| `curl.exec` | `curl_exec` | yes |
| `stream.connect` | `stream_socket_client`, `fsockopen` | no |

Missing targets are normal, not errors: without `ext-redis` the Redis hooks
simply never attach.

`curl_exec` is worth hooking even though Laravel's HTTP client already reports
timings, because plenty of code calls `curl_*` directly — SDKs, vendor
libraries, anything not routed through `Illuminate\Http\Client`. Those are
invisible at the framework layer.

## What you get back

Aggregated by type, not one record per call:

```php
'operations' => [
    'pdo.connect' => ['count' => 2, 'total_ns' => 166_400_000, 'max_ns' => 90_100_000],
    'curl.exec'   => ['count' => 7, 'total_ns' => 290_000_000, 'max_ns' => 180_000_000],
],
```

Aggregates rather than a list because the consumer's event model is
scalar-valued, and an unbounded list of records is both a memory risk and a
cardinality risk. The individual calls do still appear in the crash
[breadcrumb ring](crashes.md).

The extension attaches no semantics. It does not know which database, which
host, or which connection name — `db.system`, `server.address`,
`db.connection.name` and everything like them belong to the consumer, which has
the context to name them and the redaction rules to sanitise them.

## Scope

Operations are only aggregated while a unit of work is active. A worker sitting
idle between jobs opening a connection records a breadcrumb but no aggregate —
otherwise the numbers would belong to no unit and accumulate forever.

## Where the list stops, and why

`mysqli`, native PostgreSQL, Memcached, MongoDB and every other client are
deliberately absent, and adding them on request would be the wrong instinct.

A native hook earns its place only at a **high-value opaque boundary**: a call
where userland genuinely cannot measure as well, and where the answer matters.
Connection establishment qualifies — the time goes into DNS, TCP, TLS and
authentication, none of which PHP can see and none of which a sampler catches.
`curl_exec` qualifies because plenty of code bypasses the framework's HTTP
client entirely.

Query execution does not qualify: Laravel already times queries accurately from
events, and a native hook would add overhead to duplicate it less well. Neither
does anything a userland wrapper can bracket just as precisely.

The test is not "can we hook it" — we can hook almost anything — but "does
hooking it tell you something userland could not". Answering yes too often is
how a bounded extension turns into an APM agent.

## Known limits

**A fatal error mid-call loses that measurement.** `zend_bailout` longjmps past
the end of the wrapper, so an operation that dies in flight never records its
end. The nesting stack is bounded and reset at every unit boundary, so this
costs one measurement and never correctness. Closing the gap would mean a
`setjmp` around every `curl_exec`, which is a worse trade than losing the timing
of a call that killed the request anyway.

**Connection establishment is one number.** `pdo.connect` is total time inside
the constructor. Splitting it into DNS, TCP, TLS and authentication is not
possible from outside the driver, and inventing a breakdown we cannot measure
would be worse than not having one.

**cURL already knows more than we do.** For requests that go through a client
exposing `curl_getinfo()` transfer statistics, those are strictly better — real
DNS, connect, TLS and TTFB phases. This hook is for coverage of the calls that
do not, not a replacement for them.

## Why handler swaps rather than the Zend Observer

Because the observer costs ~20% on call-heavy code that never calls an observed
function, and the handler swap costs nothing measurable. That was a benchmark,
not a preference — see
[decision 0001](../decisions/0001-operation-hook-mechanism.md).
