<?php

/**
 * One (mode, workload) measurement. Prints a single JSON line so the runner can
 * collect results without parsing prose.
 *
 * Modes:
 *   baseline   nothing loaded
 *   loaded     extension loaded, never called — the always-on cost
 *   unit       begin/finish around each iteration, no profiling
 *   profile    begin/finish with the profiler running
 *   excimer    ext-excimer profiling at the same period, for comparison
 *
 * Two timing windows, because they answer different questions:
 *
 *   hot path    only the application work is timed. This is the cost imposed
 *               while your code runs — what a p99 latency budget cares about.
 *   end to end  begin() and finish() are inside the window, so materialising
 *               the profile into PHP is counted. This is the cost of actually
 *               keeping a profile, paid once per retained unit.
 *
 * Retaining and discarding are measured separately: tail retention only works
 * if discarding is genuinely cheap.
 */

declare(strict_types=1);

require __DIR__ . '/workloads.php';

$mode = $argv[1] ?? 'baseline';
$name = $argv[2] ?? 'cpu';
$iterations = (int) ($argv[3] ?? 5);
$periodUs = (int) ($argv[4] ?? 1000);

$workloads = cbox_bench_workloads();

if (!isset($workloads[$name])) {
    fwrite(STDERR, "unknown workload: {$name}\n");
    exit(1);
}

$work = $workloads[$name];

// Warm up: let the JIT and the allocator settle before anything is timed.
$work();

$endToEnd = str_starts_with($mode, 'e2e');
$retain = $mode === 'e2e_retain';

if ($endToEnd) {
    $mode = 'profile';
}

$samples = [];

for ($i = 0; $i < $iterations; $i++) {
    $handle = 0;
    $excimer = null;
    $outerStart = hrtime(true);

    if ($mode === 'unit') {
        $handle = cbox_telemetry_begin(['unit' => 'http', 'profile' => false]);
    } elseif ($mode === 'profile') {
        $handle = cbox_telemetry_begin(['unit' => 'http', 'profile' => true, 'period_us' => $periodUs]);
    } elseif ($mode === 'excimer') {
        $excimer = new ExcimerProfiler();
        $excimer->setPeriod($periodUs / 1_000_000);
        $excimer->setEventType(EXCIMER_CPU);
        $excimer->start();
    }

    $started = hrtime(true);
    $work();
    $elapsed = hrtime(true) - $started;

    if ($mode === 'unit' || $mode === 'profile') {
        cbox_telemetry_finish($handle, $endToEnd ? $retain : $mode === 'profile');
    } elseif ($mode === 'excimer' && $excimer !== null) {
        $excimer->stop();
        $excimer->getLog()->aggregateByFunction();
    }

    $samples[] = $endToEnd ? hrtime(true) - $outerStart : $elapsed;
}

sort($samples);
$median = $samples[intdiv(count($samples), 2)];

echo json_encode([
    'mode' => $mode,
    'workload' => $name,
    'median_ms' => round($median / 1_000_000, 3),
    'min_ms' => round($samples[0] / 1_000_000, 3),
    'max_ms' => round($samples[count($samples) - 1] / 1_000_000, 3),
]), "\n";
