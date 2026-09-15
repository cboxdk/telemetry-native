--TEST--
cbox_telemetry: PDO connection establishment is timed natively
--EXTENSIONS--
cbox_telemetry
--SKIPIF--
<?php if (!extension_loaded('pdo_sqlite')) die('skip pdo_sqlite not available'); ?>
--INI--
cbox_telemetry.profiler.allow_fallback_backend=1
cbox_telemetry.hooks.pdo=1
--FILE--
<?php
$handle = cbox_telemetry_begin(['unit' => 'http']);

$first = new PDO('sqlite::memory:');
$second = new PDO('sqlite::memory:');
$first->query('SELECT 1');

$result = cbox_telemetry_finish($handle);

var_dump(isset($result['operations']['pdo.connect']));
var_dump($result['operations']['pdo.connect']['count'] === 2);
var_dump($result['operations']['pdo.connect']['total_ns'] > 0);
var_dump($result['operations']['pdo.connect']['max_ns'] <= $result['operations']['pdo.connect']['total_ns']);

// Queries are not connections; Laravel already sees those.
var_dump(count($result['operations']) === 1);

// Connections opened outside a unit are not attributed to one.
$third = new PDO('sqlite::memory:');
$handle = cbox_telemetry_begin();
$result = cbox_telemetry_finish($handle);
var_dump($result['operations']);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
array(0) {
}
