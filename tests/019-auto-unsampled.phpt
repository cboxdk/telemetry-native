--TEST--
cbox_telemetry: adopting an automatic unit as unsampled throws the profile away
--EXTENSIONS--
cbox_telemetry
--INI--
cbox_telemetry.profiler.allow_fallback_backend=1
cbox_telemetry.auto=1
cbox_telemetry.profiler.period_us=200
--FILE--
<?php
function cbox_burn(): void
{
    for ($i = 0; $i < 300000; $i++) {
        sqrt($i);
    }
}

cbox_burn();

// The framework decides this request is not sampled after all.
$handle = cbox_telemetry_begin(['unit' => 'http', 'sampled' => false]);

cbox_burn();

$result = cbox_telemetry_finish($handle, true);

var_dump($result['sampled']);
var_dump($result['profiling']);
var_dump($result['profile']);
var_dump($result['counters']['profiler.samples']);

// A second begin() after adoption is an ordinary nested begin: new unit.
$second = cbox_telemetry_begin(['unit' => 'queue']);
var_dump($second !== $handle);
var_dump(cbox_telemetry_finish($second)['automatic']);
?>
--EXPECT--
bool(false)
bool(false)
NULL
int(0)
bool(true)
bool(false)
