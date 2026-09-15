--TEST--
cbox_telemetry: draining with nothing to drain is not an error
--EXTENSIONS--
cbox_telemetry
--INI--
cbox_telemetry.crash.enabled=1
cbox_telemetry.crash.dir=/tmp/cbox-telemetry-drain-empty
--FILE--
<?php
var_dump(cbox_telemetry_drain_crashes());
var_dump(cbox_telemetry_drain_crashes(1));
var_dump(cbox_telemetry_drain_crashes(0));
var_dump(cbox_telemetry_drain_crashes(-1));

$status = cbox_telemetry_status();
var_dump($status['crash_recorder']);
// One sink per process, named for its pid.
var_dump((string) $status['crash_path'] === '/tmp/cbox-telemetry-drain-empty/crash-' . getmypid() . '.bin');

@array_map('unlink', glob('/tmp/cbox-telemetry-drain-empty/*') ?: []);
@rmdir('/tmp/cbox-telemetry-drain-empty');
?>
--EXPECT--
array(0) {
}
array(0) {
}
array(0) {
}
array(0) {
}
string(5) "armed"
bool(true)
