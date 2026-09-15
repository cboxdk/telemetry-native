--TEST--
cbox_telemetry: begin/finish lifecycle, unknown handles and re-entry
--EXTENSIONS--
cbox_telemetry
--INI--
cbox_telemetry.profiler.allow_fallback_backend=1
--FILE--
<?php
$handle = cbox_telemetry_begin(['unit' => 'queue']);
var_dump($handle > 0);
var_dump(cbox_telemetry_status()['unit_handle'] === $handle);

// An unknown handle is not an error — the caller just gets nothing.
var_dump(cbox_telemetry_finish($handle + 1000));

$result = cbox_telemetry_finish($handle);
var_dump($result['unit']);
var_dump($result['duration_ns'] >= 0);
var_dump(array_key_exists('operations', $result), array_key_exists('counters', $result));
var_dump($result['profile']);

// Finishing twice yields nothing the second time.
var_dump(cbox_telemetry_finish($handle));
var_dump(cbox_telemetry_status()['unit_handle']);

// Units do not nest: the second begin abandons the first rather than stacking.
$first = cbox_telemetry_begin();
$second = cbox_telemetry_begin();
var_dump($second !== $first);
var_dump(cbox_telemetry_finish($first));
var_dump(cbox_telemetry_finish($second) !== []);
?>
--EXPECT--
bool(true)
bool(true)
array(0) {
}
string(5) "queue"
bool(true)
bool(true)
bool(true)
NULL
array(0) {
}
int(0)
bool(true)
array(0) {
}
bool(true)
