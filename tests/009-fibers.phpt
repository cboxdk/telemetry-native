--TEST--
cbox_telemetry: sampling survives Fiber suspend and resume
--EXTENSIONS--
cbox_telemetry
--INI--
cbox_telemetry.profiler.period_us=200
--FILE--
<?php
function cbox_fiber_work(): float
{
    $total = 0.0;

    for ($i = 0; $i < 60000; $i++) {
        $total += sqrt($i);
    }

    return $total;
}

$handle = cbox_telemetry_begin(['profile' => true]);

for ($round = 0; $round < 20; $round++) {
    $fiber = new Fiber(function (): void {
        cbox_fiber_work();
        Fiber::suspend();
        cbox_fiber_work();
    });

    $fiber->start();
    cbox_fiber_work();
    $fiber->resume();
}

$profile = cbox_telemetry_finish($handle, true, true)['profile'];
$functions = array_column($profile['frames'], 'function');

var_dump($profile['sample_count'] > 0);
var_dump($profile['dropped'] === 0);
var_dump(in_array('cbox_fiber_work', $functions, true));
echo "survived\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
survived
