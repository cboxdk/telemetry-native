--TEST--
cbox_telemetry: automatic instrumentation opens a unit before any caller does
--EXTENSIONS--
cbox_telemetry
--INI--
cbox_telemetry.profiler.allow_fallback_backend=1
cbox_telemetry.auto=1
cbox_telemetry.profiler.period_us=200
--FILE--
<?php
// A unit is already open — nobody called begin().
$status = cbox_telemetry_status();
var_dump($status['auto']);
var_dump($status['unit_handle'] > 0);
var_dump($status['unit_automatic']);

// The SAPI guess: this runs under CLI.
/*
 * Bounded by elapsed time rather than iterations: the fallback timer backend
 * samples wall clock at the scheduler's granularity, and a fixed loop can
 * finish before its first tick on a fast machine.
 */
function cbox_auto_work(float $ms): void
{
    $until = microtime(true) + $ms / 1000;

    while (microtime(true) < $until) {
        for ($i = 0; $i < 5000; $i++) {
            sqrt($i);
        }
    }
}

cbox_auto_work(150);

// Handle 0 means "whatever is open", which is what a terminate hook has.
$result = cbox_telemetry_finish(0, true);

var_dump($result['automatic']);
var_dump($result['unit']);
var_dump($result['profile']['sample_count'] > 0);
var_dump(cbox_telemetry_status()['unit_handle']);

// Once finished, there is nothing left to collect.
var_dump(cbox_telemetry_finish(0));
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
string(7) "command"
bool(true)
int(0)
array(0) {
}
