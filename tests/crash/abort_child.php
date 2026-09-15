<?php

/**
 * Crash fixture. Runs in an isolated child process — never in the test
 * process itself — announces that it is inside a unit of work, and then waits
 * to be killed by the parent.
 */

declare(strict_types=1);

cbox_telemetry_begin([
    'unit' => 'queue',
    'trace_id' => '4bf92f3577b34da6a3ce929d0e0e4736',
    'span_id' => '00f067aa0ba902b7',
]);

// A couple of breadcrumbs so the record has something to say about the past.
if (extension_loaded('pdo_sqlite')) {
    new PDO('sqlite::memory:');
}

echo "ready\n";
flush();

// The parent signals us here. Bounded so a failed test cannot hang CI.
sleep(30);

echo "not reached\n";
