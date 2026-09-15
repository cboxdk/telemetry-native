--TEST--
cbox_telemetry: disabled hooks are not registered at all
--EXTENSIONS--
cbox_telemetry
--SKIPIF--
<?php if (!extension_loaded('pdo_sqlite')) die('skip pdo_sqlite not available'); ?>
--INI--
cbox_telemetry.profiler.allow_fallback_backend=1
cbox_telemetry.hooks.pdo=0
cbox_telemetry.hooks.redis=0
cbox_telemetry.hooks.curl=0
cbox_telemetry.hooks.streams=0
--FILE--
<?php
$status = cbox_telemetry_status();
var_dump($status['hooks']);
var_dump($status['hooks_armed']);

$handle = cbox_telemetry_begin();
$pdo = new PDO('sqlite::memory:');
$result = cbox_telemetry_finish($handle);

var_dump($result['operations']);
?>
--EXPECT--
string(4) "none"
bool(false)
array(0) {
}
