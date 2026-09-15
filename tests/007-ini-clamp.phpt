--TEST--
cbox_telemetry: out-of-range configuration is clamped, never trusted
--EXTENSIONS--
cbox_telemetry
--INI--
cbox_telemetry.profiler.period_us=1
cbox_telemetry.profiler.max_depth=99999
cbox_telemetry.profiler.max_frames=1
cbox_telemetry.profiler.max_nodes=1
cbox_telemetry.breadcrumbs.size=999999
--FILE--
<?php
$limits = cbox_telemetry_status()['limits'];

var_dump($limits['period_us'] === 100);
var_dump($limits['max_depth'] === 256);
var_dump($limits['max_frames'] === 64);
var_dump($limits['max_nodes'] === 256);
var_dump($limits['breadcrumbs'] <= 4096);

// Per-unit overrides are clamped the same way.
$handle = cbox_telemetry_begin(['profile' => true, 'period_us' => 999999999, 'max_depth' => -5]);
for ($i = 0; $i < 20000; $i++) { sqrt($i); }
$result = cbox_telemetry_finish($handle, true);

var_dump($result['counters']['profiler.period_ns'] === 100000000);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
