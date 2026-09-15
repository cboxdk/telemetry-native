---
title: "The profiler"
weight: 22
description: "Statistical CPU sampling: the safe-point stack walk, the aggregation, and what the numbers mean."
---

# The profiler

A statistical sampler. It interrupts PHP at a fixed rate and records what was on
the stack — it does not instrument function calls, so the cost does not scale
with how much code you run.

## The sampling path

```
timer fires
   ↓ signal handler: one atomic add, one atomic store to EG(vm_interrupt).
   ↓ nothing else — nothing else would be async-signal-safe.
engine reaches a safe point, calls our interrupt function
   ↓ take the pending tick count, walk EG(current_execute_data)
   ↓ intern each frame, fold the path into the call trie
   ↓ arena only: no emalloc, no zval, no refcounting
```

The split matters. Walking the PHP stack inside a signal handler means reading
engine state that may be half-written, which is how profilers produce bug
reports nobody can reproduce. Doing it at the VM's own safe point means the
engine is, by definition, in a consistent state.

The cost is that a sample cannot land *inside* a long internal call — no safe
point is reached until it returns. The ticks are not lost: the Linux backend
reads the timer's overrun count and attributes all of them when control comes
back. But CPU burned inside one long `curl_exec` shows up attributed at its
return rather than spread across it, which is exactly why exact
[operation timing](operations.md) exists alongside sampling.

## Frames

A frame is one *function*: name, declaring file, declaration line. Not one call
site, and specifically not the currently-executing line — sample lines change
constantly and would blow the frame table up within a single request.

Names are copied into the arena rather than referenced as `zend_string*`,
because a non-interned string can be freed and its address reused before the
profile is materialised.

Using the declaration line rather than the current opline has a second benefit:
it is immune to the tracing JIT, where the current opline can be stale.

## Aggregation

Samples fold into a trie of call paths as they are taken: each node is
(parent, frame) with a self-sample counter, and the path from a node to the root
is the stack. A 30-second profile therefore occupies the same memory as a
30-millisecond one — memory is a function of how many *distinct* call paths the
code has, not of how long you sampled.

Past `max_nodes` or `max_frames`, further samples increment `profiler.dropped`
rather than allocating. Stacks deeper than `max_depth` are recorded under a
synthetic `<truncated>` frame, so a truncated profile looks truncated instead of
looking like a shallow one.

## Reading the output

```php
'profile' => [
    'sample_count'  => 814,
    'period_ns'     => 1_000_000,
    'clock'         => 'cpu',
    'dropped'       => 0,
    'frames'        => [ ['function' => …, 'file' => …, 'line' => …], … ],
    'top_functions' => [ ['frame_id' => 1, 'samples' => 284], … ],
    'stacks'        => [ [parentNode, frameId, samples], … ] | null,
]
```

- **`top_functions`** is *self* samples per function, descending — time spent in
  that function itself, not in what it called. A function that only delegates
  will sit near zero, and should.
- **`frames`** is a list indexed by frame id. Emitted once, referenced by id
  everywhere else; no string is ever repeated.
- **`stacks`** is only present when you pass `includeStacks: true`. Each entry
  is `[parent_node_id, frame_id, self_samples]`, and a node's own id is its
  position plus one — node 0 is the root and is never emitted. That is enough to
  rebuild the call tree, a flame graph, or a speedscope/pprof export, all of
  which belong outside the extension.
- **`clock`** is `cpu` or `wall`. Do not assume the first one; the fallback
  backend genuinely cannot measure CPU time and says so rather than pretending.

`sample_count` is the sum of `top_functions` samples, always.

### Attribution of internal calls moves between PHP versions

Where a sample taken during an internal call lands is the engine's business,
not ours, and the engine changed its mind. The same `sqrt()` in a loop:

| | top functions |
|---|---|
| PHP 8.3 | the *calling* userland function |
| PHP 8.4, 8.5 | `sqrt` itself, with the caller below it |

Neither is wrong, and we do not normalise it — inventing a frame the engine did
not report would be worse than reporting what it did. The call tree is stable
across both: the path is the same, only which node owns the self time differs.
So if you are comparing profiles across a PHP upgrade, or asserting on them in
a test, use inclusive time from `stacks` rather than `top_functions`.

## Choosing a period

The default is 1 ms, and the range is clamped to 100 µs – 100 ms. Faster is not
obviously better: the sampling itself is cheap, but a rate high enough to
resolve a 2 ms function is also high enough to make a profile of a 2-second
request enormous and no more actionable.

On Linux the measured period matches the requested one closely (1 ms requested →
1.00 ms measured). On the fallback backend it is the requested period plus
scheduling latency, which at 1 ms is roughly a 2× error — usable for finding the
hot function, not for absolute timings.

## What it does not do

It is not a general-purpose profiler and has no ambition to be. There is no
allocation profiling, no line-level attribution, no wall/CPU switching at
runtime, and no export format. Anything that wants a flame graph should build
one from `stacks` outside the extension.
