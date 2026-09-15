--TEST--
cbox_telemetry: an automatic unit nobody closes stops sampling instead of running forever
--EXTENSIONS--
cbox_telemetry
--INI--
cbox_telemetry.profiler.allow_fallback_backend=1
cbox_telemetry.auto=1
cbox_telemetry.profiler.period_us=100
cbox_telemetry.auto_max_ms=1000
--FILE--
<?php
// Stands in for a queue worker: an automatic unit that is never finished,
// because nothing in the process knows about units of work.
$deadline = microtime(true) + 2.5;

while (microtime(true) < $deadline) {
    for ($i = 0; $i < 20000; $i++) {
        sqrt($i);
    }
}

$result = cbox_telemetry_finish(0, true);

var_dump($result['counters']['profiler.capped']);

// Sampling stopped, but what it collected before the valve tripped is kept.
var_dump($result['profile']['sample_count'] > 0);

// An explicit unit gets no deadline — its owner is going to call finish().
$handle = cbox_telemetry_begin(['unit' => 'queue']);
$result = cbox_telemetry_finish($handle);
var_dump($result['counters']['profiler.capped']);
?>
--EXPECT--
bool(true)
bool(true)
bool(false)
