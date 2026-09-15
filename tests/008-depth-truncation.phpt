--TEST--
cbox_telemetry: stacks deeper than max_depth are marked, not silently cut
--EXTENSIONS--
cbox_telemetry
--INI--
cbox_telemetry.profiler.allow_fallback_backend=1
cbox_telemetry.profiler.period_us=200
cbox_telemetry.profiler.max_depth=8
--FILE--
<?php
function cbox_deep(int $depth): float
{
    if ($depth > 0) {
        return cbox_deep($depth - 1);
    }

    $total = 0.0;

    for ($i = 0; $i < 300000; $i++) {
        $total += sqrt($i);
    }

    return $total;
}

$handle = cbox_telemetry_begin(['profile' => true]);
cbox_deep(40);
$profile = cbox_telemetry_finish($handle, true, true)['profile'];

$functions = array_column($profile['frames'], 'function');

var_dump($profile['sample_count'] > 0);
var_dump(in_array('<truncated>', $functions, true));

// No recorded path is longer than the configured depth (plus the marker).
$nodes = [0 => ['parent' => -1]];
foreach ($profile['stacks'] as $index => [$parent, $frameId, $samples]) {
    $nodes[$index + 1] = ['parent' => $parent];
}

$deepest = 0;
foreach (array_keys($nodes) as $id) {
    $length = 0;
    while ($id > 0) {
        $id = $nodes[$id]['parent'];
        $length++;
    }
    $deepest = max($deepest, $length);
}

var_dump($deepest <= 9);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
