<?php

/**
 * Benchmark runner. Spawns one clean PHP process per (mode, workload) so no
 * mode can contaminate another, then prints overhead relative to the baseline.
 *
 * Usage: php benchmarks/run.php <path-to-extension.so> [iterations] [period_us]
 */

declare(strict_types=1);

$extension = $argv[1] ?? null;
$iterations = (int) ($argv[2] ?? 7);
$periodUs = (int) ($argv[3] ?? 1000);

if ($extension === null || !is_file($extension)) {
    fwrite(STDERR, "usage: php run.php /path/to/cbox_telemetry.so [iterations] [period_us]\n");
    exit(1);
}

$hasExcimer = trim((string) shell_exec(PHP_BINARY . ' -m 2>/dev/null | grep -ix excimer')) !== '';

$hooksOff = [
    '-d', 'cbox_telemetry.hooks.pdo=0',
    '-d', 'cbox_telemetry.hooks.redis=0',
    '-d', 'cbox_telemetry.hooks.curl=0',
];

$hooksOn = [
    '-d', 'cbox_telemetry.hooks.pdo=1',
    '-d', 'cbox_telemetry.hooks.redis=1',
    '-d', 'cbox_telemetry.hooks.curl=1',
];

$load = ['-d', 'extension=' . $extension];

/*
 * Every mode states its hooks explicitly. Leaving them at the INI default made
 * an earlier run attribute the hook mechanism's cost to begin/finish.
 *
 * [php flags, what overhead.php should do]
 */
$modes = [
    'baseline'      => [[], 'baseline'],
    'loaded'        => [array_merge($load, $hooksOff), 'loaded'],
    'hooks'         => [array_merge($load, $hooksOn), 'loaded'],
    'unit'          => [array_merge($load, $hooksOff), 'unit'],
    'profile'       => [array_merge($load, $hooksOff), 'profile'],
    'profile+hooks' => [array_merge($load, $hooksOn), 'profile'],
];

// Window includes begin()/finish(), so materialisation is counted.
$modes['e2e discard'] = [array_merge($load, $hooksOff), 'e2e_discard'];
$modes['e2e retain'] = [array_merge($load, $hooksOff), 'e2e_retain'];

if ($hasExcimer) {
    $modes['excimer'] = [['-d', 'extension=excimer.so'], 'excimer'];
}

$workloads = ['cpu', 'calls', 'deep', 'internal', 'mixed'];
$results = [];

foreach ($modes as $mode => [$flags, $runMode]) {
    foreach ($workloads as $workload) {
        $command = array_merge(
            [PHP_BINARY, '-n'],
            $flags,
            [__DIR__ . '/overhead.php', $runMode, $workload, (string) $iterations, (string) $periodUs]
        );

        $output = [];
        exec(implode(' ', array_map('escapeshellarg', $command)) . ' 2>/dev/null', $output);
        $decoded = json_decode((string) end($output), true);

        if (!is_array($decoded)) {
            fwrite(STDERR, "failed: {$mode}/{$workload}\n");
            continue;
        }

        $results[$mode][$workload] = $decoded['median_ms'];
    }
}

$pad = 14;
printf("%-{$pad}s", 'mode');
foreach ($workloads as $workload) {
    printf('%12s', $workload);
}
echo "\n" . str_repeat('-', $pad + 12 * count($workloads)) . "\n";

foreach ($results as $mode => $row) {
    printf("%-{$pad}s", $mode);
    foreach ($workloads as $workload) {
        $value = $row[$workload] ?? null;
        printf('%12s', $value === null ? '-' : number_format($value, 1));
    }
    echo "\n";
}

echo "\noverhead vs baseline (median wall time, lower is better)\n";
printf("%-{$pad}s", 'mode');
foreach ($workloads as $workload) {
    printf('%12s', $workload);
}
echo "\n" . str_repeat('-', $pad + 12 * count($workloads)) . "\n";

foreach ($results as $mode => $row) {
    if ($mode === 'baseline') {
        continue;
    }

    printf("%-{$pad}s", $mode);

    foreach ($workloads as $workload) {
        $base = $results['baseline'][$workload] ?? null;
        $value = $row[$workload] ?? null;

        if ($base === null || $value === null || $base <= 0.0) {
            printf('%12s', '-');
            continue;
        }

        printf('%11s%%', number_format(($value - $base) / $base * 100, 1));
    }

    echo "\n";
}
