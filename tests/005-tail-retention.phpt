--TEST--
cbox_telemetry: a profile is only materialised when it is asked for
--EXTENSIONS--
cbox_telemetry
--INI--
cbox_telemetry.profiler.allow_fallback_backend=1
--FILE--
<?php
function cbox_work(): void
{
    for ($i = 0; $i < 50000; $i++) {
        sqrt($i);
    }
}

// Profiling off entirely: nothing sampled, nothing returned.
$handle = cbox_telemetry_begin(['profile' => false]);
cbox_work();
$result = cbox_telemetry_finish($handle, true);
var_dump($result['profiling'], $result['profile'], $result['counters']['profiler.samples']);

// Profiling on, but the caller decides the unit was not interesting: samples
// were taken natively, no PHP array is ever built.
$handle = cbox_telemetry_begin(['profile' => true]);
cbox_work();
$result = cbox_telemetry_finish($handle, false);
var_dump($result['profiling'], $result['profile']);

// Unsampled units never start the profiler at all.
$handle = cbox_telemetry_begin(['profile' => true, 'sampled' => false]);
cbox_work();
$result = cbox_telemetry_finish($handle, true);
var_dump($result['sampled'], $result['profiling'], $result['profile']);
?>
--EXPECT--
bool(false)
NULL
int(0)
bool(true)
NULL
bool(false)
bool(false)
NULL
