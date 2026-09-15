<?php

/**
 * A request shaped like a real one: some CPU, a connection, some queries, some
 * array and string work. Not a micro-benchmark — the point is to measure the
 * extension against a load that looks like production, not against an empty
 * loop where any fixed cost dominates.
 */

declare(strict_types=1);

$started = hrtime(true);

$total = 0.0;

for ($i = 1; $i < 30000; $i++) {
    $total += sqrt($i) * log($i);
}

$pdo = new PDO('sqlite::memory:');
$pdo->exec('CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, score REAL)');

$insert = $pdo->prepare('INSERT INTO t (name, score) VALUES (?, ?)');

for ($i = 0; $i < 200; $i++) {
    $insert->execute(['row-' . $i, $i * 1.5]);
}

$rows = $pdo->query('SELECT * FROM t ORDER BY score DESC')->fetchAll(PDO::FETCH_ASSOC);

usort($rows, static fn (array $a, array $b): int => strcmp((string) $a['name'], (string) $b['name']));

$durationMs = (hrtime(true) - $started) / 1e6;

$native = null;

if (function_exists('cbox_telemetry_finish')) {
    // Automatic instrumentation opened the unit; this is the terminate hook.
    // Keep the profile only for slow requests, as a consumer would.
    $result = cbox_telemetry_finish(0, $durationMs >= 40);
    $native = $result === [] ? null : [
        'samples' => $result['counters']['profiler.samples'] ?? 0,
        'deferred' => $result['counters']['profiler.deferred_samples'] ?? 0,
        'overruns' => $result['counters']['profiler.timer_overruns'] ?? 0,
        'kept' => $result['profile'] !== null,
    ];
}

header('Content-Type: application/json');

echo json_encode([
    'ok' => true,
    'rows' => count($rows),
    'duration_ms' => round($durationMs, 2),
    'pid' => getmypid(),
    'native' => $native,
]), "\n";
