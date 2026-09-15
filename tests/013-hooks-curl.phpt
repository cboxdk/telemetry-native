--TEST--
cbox_telemetry: curl_exec is timed natively, including failures
--EXTENSIONS--
cbox_telemetry
--SKIPIF--
<?php if (!extension_loaded('curl')) die('skip ext-curl not available'); ?>
--INI--
cbox_telemetry.hooks.curl=1
--FILE--
<?php
$handle = cbox_telemetry_begin(['unit' => 'http']);

// Port 1 on loopback: refused immediately, no network needed.
foreach ([0, 1] as $ignored) {
    $curl = curl_init('http://127.0.0.1:1/');
    curl_setopt($curl, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($curl, CURLOPT_CONNECTTIMEOUT, 1);
    curl_exec($curl);
    unset($curl);
}

$result = cbox_telemetry_finish($handle);

var_dump($result['operations']['curl.exec']['count'] === 2);
var_dump($result['operations']['curl.exec']['total_ns'] > 0);
var_dump($result['counters']['ops.overflow'] === 0);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
