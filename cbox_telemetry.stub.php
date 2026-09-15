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
 * Recognised context keys: trace_id (32 hex), span_id (16 hex),
 * unit ("http"|"queue"|"command"|"schedule"), sampled (bool),
 * profile (bool), period_us (int), max_depth (int).
 */
function cbox_telemetry_begin(array $context = []): int {}

/**
 * End the unit and return its aggregates. Returns an empty array when the
 * handle is not the active one.
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
