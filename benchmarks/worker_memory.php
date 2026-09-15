<?php

/**
 * Long-running worker memory stability.
 *
 * A queue worker runs thousands of units in one process. Anything that grows
 * per unit shows up here and nowhere else — a per-request benchmark would
 * never see it.
 *
 * Usage: php -d extension=… benchmarks/worker_memory.php [units] [period_us]
 */

declare(strict_types=1);

$units = (int) ($argv[1] ?? 20000);
$periodUs = (int) ($argv[2] ?? 1000);

function cbox_worker_job(int $seed): float
{
    $total = 0.0;

    // Distinct call paths per job, so frame and node tables are exercised
    // rather than hitting the same handful of entries every time.
    for ($i = 0; $i < 2000; $i++) {
        $total += sqrt($i + $seed);
    }

    return $total;
}

function cbox_rss_kb(): int
{
    $out = @shell_exec('ps -o rss= -p ' . getmypid());

    return (int) trim((string) $out);
}

$start = cbox_rss_kb();
$checkpoints = [];

for ($unit = 1; $unit <= $units; $unit++) {
    $handle = cbox_telemetry_begin([
        'unit' => 'queue',
        'profile' => true,
        'period_us' => $periodUs,
    ]);

    cbox_worker_job($unit);

    // One in fifty is "slow" and keeps its profile, as in a real worker.
    cbox_telemetry_finish($handle, $unit % 50 === 0);

    if ($unit % max(1, intdiv($units, 10)) === 0) {
        $checkpoints[$unit] = cbox_rss_kb();
    }
}

printf("units: %d, period: %d us%s", $units, $periodUs, PHP_EOL);
printf("RSS at start: %d KB%s", $start, PHP_EOL);

foreach ($checkpoints as $unit => $rss) {
    printf("  after %7d units: %7d KB  (%+d KB)%s", $unit, $rss, $rss - $start, PHP_EOL);
}

$status = cbox_telemetry_status();
printf("arena peak: %d bytes, dropped: %d, PHP heap: %d bytes%s",
    $status['arena_peak_bytes'], $status['profiler_dropped'], memory_get_usage(true), PHP_EOL);
