--TEST--
cbox_telemetry: an isolated fatal signal produces a parseable crash record
--EXTENSIONS--
cbox_telemetry
--SKIPIF--
<?php
if (!function_exists('proc_open')) die('skip proc_open disabled');
if (stripos(PHP_OS_FAMILY, 'win') === 0) die('skip POSIX only');
?>
--INI--
cbox_telemetry.crash.enabled=1
cbox_telemetry.crash.dir=/tmp/cbox-telemetry-test-abort
--FILE--
<?php
/*
 * The sink path is PHP_INI_SYSTEM on purpose — it is read once, at MINIT,
 * long before any request can ask for it — so parent and child are pointed at
 * the same directory through the INI block above and -d below, not ini_set().
 */
$dir = '/tmp/cbox-telemetry-test-abort';

foreach (glob($dir . '/*') ?: [] as $stale) {
    @unlink($stale);
}

$command = [
    PHP_BINARY,
    '-n',
    '-d', 'extension_dir=' . ini_get('extension_dir'),
    '-d', 'extension=cbox_telemetry.so',
    '-d', 'cbox_telemetry.crash.enabled=1',
    '-d', 'cbox_telemetry.crash.dir=' . $dir,
    __DIR__ . '/crash/abort_child.php',
];

$pipes = [];
$process = proc_open($command, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes);

if (!is_resource($process)) {
    die("could not start the child\n");
}

// Wait until the child is inside its unit of work before killing it.
$ready = fgets($pipes[1]);
var_dump(trim((string) $ready) === 'ready');

proc_terminate($process, defined('SIGABRT') ? SIGABRT : 6);

$status = proc_get_status($process);
$waited = 0;

while ($status['running'] && $waited < 5_000_000) {
    usleep(10_000);
    $waited += 10_000;
    $status = proc_get_status($process);
}

fclose($pipes[1]);
fclose($pipes[2]);
proc_close($process);

// The child must actually be dead, not merely interrupted.
var_dump($status['running'] === false);
var_dump($status['signaled'] === true || $status['exitcode'] !== 0);

// Read the record back through the extension that wrote it.
$records = cbox_telemetry_drain_crashes();

if (count($records) !== 1) {
    echo "expected exactly one record, got " . count($records) . "\n";
    echo file_exists($dir . '/crashes.bin')
        ? "sink size: " . filesize($dir . '/crashes.bin') . "\n"
        : "no sink written\n";
}

$record = $records[0] ?? [];

var_dump($record['signal_name'] ?? null);
var_dump(($record['pid'] ?? 0) > 0);
var_dump($record['unit'] ?? null);
var_dump($record['trace_id'] ?? null);
var_dump($record['span_id'] ?? null);
var_dump(($record['timestamp_ns'] ?? 0) > 0);
var_dump(count($record['breadcrumbs'] ?? []) > 0);
var_dump($record['breadcrumbs'][0]['type'] ?? null);
var_dump($record['breadcrumbs'][0]['label'] ?? null);

// Draining consumes: a second drain finds nothing.
var_dump(cbox_telemetry_drain_crashes());

@array_map('unlink', glob($dir . '/*') ?: []);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
string(7) "SIGABRT"
bool(true)
string(5) "queue"
string(32) "4bf92f3577b34da6a3ce929d0e0e4736"
string(16) "00f067aa0ba902b7"
bool(true)
bool(true)
string(10) "unit.begin"
string(5) "queue"
array(0) {
}
