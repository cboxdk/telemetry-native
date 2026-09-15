--TEST--
cbox_telemetry: profile output shape and frame interning
--EXTENSIONS--
cbox_telemetry
--INI--
cbox_telemetry.profiler.period_us=200
--FILE--
<?php
function cbox_spin(int $rounds): float
{
    $total = 0.0;

    for ($i = 1; $i < $rounds; $i++) {
        $total += sqrt($i);
    }

    return $total;
}

$handle = cbox_telemetry_begin(['profile' => true]);

for ($i = 0; $i < 300; $i++) {
    cbox_spin(2000);
}

$result = cbox_telemetry_finish($handle, true, true);
$profile = $result['profile'];

var_dump(is_array($profile));
var_dump($profile['sample_count'] > 0);
var_dump($profile['period_ns'] === 200000);
var_dump($profile['dropped'] === 0);

// The profile says which clock it was sampled against rather than implying CPU.
var_dump(in_array($profile['clock'], ['cpu', 'wall'], true));

// Frames are emitted once and referenced by id; strings are never repeated.
var_dump(count($profile['frames']) > 0);
$functions = array_column($profile['frames'], 'function');
var_dump(count($functions) === count(array_unique($functions)));
var_dump(in_array('cbox_spin', $functions, true));

foreach ($profile['top_functions'] as $entry) {
    if (!isset($profile['frames'][$entry['frame_id']]) || $entry['samples'] <= 0) {
        echo "bad top_functions entry\n";
    }
}

// Every stack node points at a frame that exists and a parent that precedes it.
foreach ($profile['stacks'] as $index => [$parent, $frameId, $samples]) {
    if ($parent > $index || !isset($profile['frames'][$frameId]) || $samples < 0) {
        echo "bad stack node\n";
    }
}

var_dump(array_sum(array_column($profile['top_functions'], 'samples')) === $profile['sample_count']);
echo "done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
done
