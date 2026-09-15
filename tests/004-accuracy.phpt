--TEST--
cbox_telemetry: samples land in the function that burns the CPU
--EXTENSIONS--
cbox_telemetry
--INI--
cbox_telemetry.profiler.allow_fallback_backend=1
cbox_telemetry.profiler.period_us=200
--FILE--
<?php
/* The spike fixture from the PRD: a() calls b(), b() does the work. */

function cbox_b(): float
{
    $total = 0.0;

    for ($i = 1; $i < 3000; $i++) {
        $total += sqrt($i);
    }

    return $total;
}

function cbox_a(int $rounds): float
{
    $total = 0.0;

    for ($i = 0; $i < $rounds; $i++) {
        $total += cbox_b();
    }

    return $total;
}

$handle = cbox_telemetry_begin(['profile' => true]);
cbox_a(400);
$profile = cbox_telemetry_finish($handle, true, true)['profile'];

$selfByName = [];

foreach ($profile['top_functions'] as $entry) {
    $selfByName[$profile['frames'][$entry['frame_id']]['function']] = $entry['samples'];
}

$total = $profile['sample_count'];
$inner = ($selfByName['cbox_b'] ?? 0) + ($selfByName['sqrt'] ?? 0);

// Nearly all self time belongs to the inner work, essentially none to the caller.
var_dump($inner / $total > 0.9);
var_dump(($selfByName['cbox_a'] ?? 0) / $total < 0.1);

// …and cbox_a must still be visible as the caller in the tree.
$frameIds = [];
foreach ($profile['frames'] as $id => $frame) {
    $frameIds[$frame['function']] = $id;
}

$nodes = [];
foreach ($profile['stacks'] as $index => [$parent, $frameId, $samples]) {
    $nodes[$index + 1] = ['parent' => $parent, 'frame' => $frameId];
}

$foundEdge = false;
foreach ($nodes as $node) {
    if ($node['frame'] === ($frameIds['cbox_b'] ?? -1)
        && isset($nodes[$node['parent']])
        && $nodes[$node['parent']]['frame'] === ($frameIds['cbox_a'] ?? -2)) {
        $foundEdge = true;
    }
}

var_dump($foundEdge);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
