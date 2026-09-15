--TEST--
cbox_telemetry: a drain that runs out of budget still makes progress
--EXTENSIONS--
cbox_telemetry
--SKIPIF--
<?php
if (!function_exists('proc_open')) die('skip proc_open disabled');
if (stripos(PHP_OS_FAMILY, 'win') === 0) die('skip POSIX only');
if (!function_exists('posix_geteuid')) die('skip ext/posix required');
?>
--INI--
cbox_telemetry.crash.enabled=1
cbox_telemetry.crash.dir=/tmp/cbox-telemetry-test-progress
--FILE--
<?php
$dir  = '/tmp/cbox-telemetry-test-progress';
$mine = $dir . '/' . posix_geteuid();
$self = $mine . '/crash-' . getmypid() . '.bin';

foreach (glob($dir . '/*/*') ?: [] as $stale) {
    if ($stale !== $self) {
        @unlink($stale);
    }
}

/*
 * One real record, produced by a child that actually crashed, replayed into a
 * single sink owned by a pid that is not running. Reaching this in production
 * takes an undrained sink plus pid reuse, which is rare — but when it happened
 * the whole file was replayed on every drain and its inode was never
 * reclaimed, so it is worth pinning down.
 */
$command = [
    PHP_BINARY, '-n',
    '-d', 'extension_dir=' . ini_get('extension_dir'),
    '-d', 'extension=cbox_telemetry.so',
    '-d', 'cbox_telemetry.crash.enabled=1',
    '-d', 'cbox_telemetry.crash.dir=' . $dir,
    __DIR__ . '/crash/abort_child.php',
];

$pipes = [];
$process = proc_open($command, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes);

if (!is_resource($process)) {
    die("could not start a child\n");
}

fgets($pipes[1]);

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

$produced = array_values(array_diff(glob($mine . '/crash-*.bin') ?: [], [$self]));

if (count($produced) !== 1) {
    die('expected one sink, got ' . count($produced) . "\n");
}

$record = file_get_contents($produced[0]);
unlink($produced[0]);

$sink = $mine . '/crash-31337.bin';
file_put_contents($sink, str_repeat($record, 3));
chmod($sink, 0600);

$counts = [];

for ($i = 0; $i < 4; $i++) {
    $counts[] = count(cbox_telemetry_drain_crashes(1));
}

echo 'drained: ', implode(' ', $counts), "\n";
echo 'sink reclaimed: ', var_export(!file_exists($sink), true), "\n";

@unlink($sink);
?>
--EXPECT--
drained: 1 1 1 0
sink reclaimed: true
