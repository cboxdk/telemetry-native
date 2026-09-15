<?php
/**
 * The spike fixture from the PRD: a() calls b() in a loop, b() burns CPU.
 * A correct profiler puts the samples in b() and shows a() as its caller with
 * very little self time.
 */

function b(): float
{
    $total = 0.0;

    for ($i = 1; $i < 4000; $i++) {
        $total += sqrt($i) * log($i);
    }

    return $total;
}

function a(int $rounds): float
{
    $total = 0.0;

    for ($i = 0; $i < $rounds; $i++) {
        $total += b();
    }

    return $total;
}

$handle = cbox_telemetry_begin(['unit' => 'http', 'sampled' => true, 'profile' => true]);

a(400);

$result = cbox_telemetry_finish($handle, includeProfile: true, includeStacks: true);
$profile = $result['profile'];

printf("duration      %.1f ms\n", $result['duration_ns'] / 1e6);
printf("samples       %d (dropped %d, period %d ns)\n",
    $profile['sample_count'], $profile['dropped'], $profile['period_ns']);
printf("frames        %d, nodes %d, arena peak %d bytes\n",
    $result['counters']['profiler.frames'],
    $result['counters']['profiler.nodes'],
    $result['counters']['arena.peak_bytes']);

echo "\ntop functions by self samples:\n";

foreach (array_slice($profile['top_functions'], 0, 8) as $entry) {
    $frame = $profile['frames'][$entry['frame_id']];
    printf("  %6d  %-40s %s:%d\n",
        $entry['samples'],
        $frame['function'],
        $frame['file'] === null ? '<internal>' : basename($frame['file']),
        $frame['line']);
}

echo "\ncall tree:\n";

$byId = [];
foreach ($profile['stacks'] as $i => [$parent, $frameId, $samples]) {
    $byId[$i + 1] = ['parent' => $parent, 'frame' => $frameId, 'samples' => $samples];
}

$render = function (int $node, int $depth) use (&$render, $byId, $profile): void {
    foreach ($byId as $id => $entry) {
        if ($entry['parent'] !== $node) {
            continue;
        }
        printf("  %s%s (self %d)\n",
            str_repeat('  ', $depth),
            $profile['frames'][$entry['frame']]['function'] ?? '?',
            $entry['samples']);
        $render($id, $depth + 1);
    }
};

$render(0, 0);
