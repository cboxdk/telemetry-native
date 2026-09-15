<?php

/**
 * @generate-function-entries
 * @undocumentable
 */

/** The extension version, e.g. "0.1.0-dev". */
function cbox_telemetry_version(): string {}

/**
 * Everything a diagnostics command needs: build mode, timer backend, active
 * hooks, crash recorder state and the current bounds. No application data.
 */
function cbox_telemetry_status(): array {}

/**
 * Begin a unit of work. Returns a handle, or 0 when the extension is disabled
 * or declined to start (which callers should treat as "no native telemetry",
 * never as an error).
 *
 * With cbox_telemetry.auto enabled, a unit is already open by the time any PHP
 * runs, and this call *adopts* it: the samples taken during bootstrap are kept,
 * and the context given here labels them. The returned handle is the automatic
 * unit's own.
 *
 * Recognised context keys: trace_id (32 hex), span_id (16 hex),
 * unit ("http"|"queue"|"command"|"schedule"), sampled (bool),
 * profile (bool), period_us (int), max_depth (int).
 *
 * On adoption, period_us and max_depth only apply if profiling was not already
 * running — re-arming the timer would discard the samples worth adopting.
 * Passing sampled or profile as false discards the profile instead.
 */
function cbox_telemetry_begin(array $context = []): int {}

/**
 * End the unit and return its aggregates. Returns an empty array when the
 * handle is not the active one.
 *
 * Pass 0 to end whichever unit is open. That is how automatic instrumentation
 * is collected: a terminate hook never saw a begin() and has no handle.
 *
 * The profile is only materialised into PHP when $includeProfile is true —
 * a fast request resets the native state and allocates nothing.
 */
function cbox_telemetry_finish(int $handle, bool $includeProfile = false, bool $includeStacks = false): array {}

/**
 * Read and remove pending crash records written by processes that died.
 * Decoding lives here so the binary format stays an implementation detail.
 */
function cbox_telemetry_drain_crashes(int $max = 32): array {}
