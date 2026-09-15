--TEST--
cbox_telemetry: status reports what was actually hooked, not what was asked for
--EXTENSIONS--
cbox_telemetry
--SKIPIF--
<?php
if (extension_loaded('redis')) die('skip needs ext-redis absent to show the gap');
if (!extension_loaded('pdo_sqlite')) die('skip pdo_sqlite not available');
?>
--INI--
cbox_telemetry.hooks.pdo=1
cbox_telemetry.hooks.redis=1
cbox_telemetry.hooks.curl=0
cbox_telemetry.hooks.streams=0
--FILE--
<?php
// Force the hooks to install (they attach on the first request).
$handle = cbox_telemetry_begin();
cbox_telemetry_finish($handle);

$status = cbox_telemetry_status();
$detail = $status['hook_detail'];

// PDO is present, so asking for it produced real hooks.
var_dump($detail['pdo']['requested']);
var_dump($detail['pdo']['installed'] > 0);
var_dump($detail['pdo']['active']);

/*
 * Redis was asked for and is not installed. Configuration alone cannot tell
 * these apart, which is the whole point: "enabled" would be a lie here.
 */
var_dump($detail['redis']['requested']);
var_dump($detail['redis']['installed']);
var_dump($detail['redis']['unavailable'] > 0);
var_dump($detail['redis']['active']);

// Not asked for at all.
var_dump($detail['curl']['requested'], $detail['curl']['active']);

// The functions actually wrapped are named.
var_dump(in_array('PDO::__construct', $status['hooks_installed'], true));

// And the profiler says why it is or is not running.
var_dump($status['profiler_status']);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
int(0)
bool(true)
bool(false)
bool(false)
bool(false)
bool(true)
string(5) "ready"
