--TEST--
cbox_telemetry: two closures in one file are two frames, not one
--EXTENSIONS--
cbox_telemetry
--INI--
cbox_telemetry.profiler.allow_fallback_backend=1
cbox_telemetry.profiler.period_us=200
--FILE--
<?php
/*
 * Before PHP 8.4 every closure in a file is called "{closure}", so a frame
 * identity of name+file alone folds them together and reports both sets of
 * samples against whichever was seen first. The declaration line is what tells
 * them apart.
 */
function cbox_spin(float $ms, callable $work): void
{
    $until = microtime(true) + $ms / 1000;

    while (microtime(true) < $until) {
        $work();
    }
}

$first = function (): float {
    $total = 0.0;

    for ($i = 0; $i < 5000; $i++) {
        $total += sqrt($i);
    }

    return $total;
};

$second = function (): float {
    $total = 0.0;

    for ($i = 0; $i < 5000; $i++) {
        $total += sqrt($i);
    }

    return $total;
};

$handle = cbox_telemetry_begin(['profile' => true]);
// Long enough that both are sampled even on the coarse wall-clock backend.
cbox_spin(250, $first);
cbox_spin(250, $second);
$profile = cbox_telemetry_finish($handle, true)['profile'];

$closures = array_values(array_filter(
    $profile['frames'],
    static fn (array $frame): bool => str_contains($frame['function'], 'closure')
));

var_dump(count($closures) === 2);

// …and they are distinguished by where they were declared.
$lines = array_column($closures, 'line');
sort($lines);
var_dump(count(array_unique($lines)) === 2);
?>
--EXPECT--
bool(true)
bool(true)
