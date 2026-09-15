--TEST--
cbox_telemetry: a sink exists only for a process that actually crashed
--EXTENSIONS--
cbox_telemetry
--SKIPIF--
<?php
if (!function_exists('proc_open')) die('skip proc_open disabled');
if (stripos(PHP_OS_FAMILY, 'win') === 0) die('skip POSIX only');
?>
--INI--
cbox_telemetry.profiler.allow_fallback_backend=1
cbox_telemetry.crash.enabled=1
cbox_telemetry.crash.dir=/tmp/cbox-telemetry-test-multi
--FILE--
<?php
$dir = '/tmp/cbox-telemetry-test-multi';
$mine = $dir . '/crash-' . getmypid() . '.bin';

/*
 * Clear leftovers, but not our own sink: it is already open, and unlinking it
 * would leave this process writing into a file with no name.
 */
foreach (glob($dir . '/*') ?: [] as $stale) {
    if ($stale !== $mine) {
        @unlink($stale);
    }
}

// The path is reserved for this process, but nothing is written until there is
// something to write — a process that never crashes leaves no file behind.
var_dump(cbox_telemetry_status()['crash_path'] === $mine);
var_dump(file_exists($mine));

function cbox_spawn_crasher(string $dir): array
{
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

    return [$process, $pipes];
}

// Two crashing children, so two separate sinks.
$children = [cbox_spawn_crasher($dir), cbox_spawn_crasher($dir)];

foreach ($children as [$process, $pipes]) {
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
}

// One file per crashed process, and none for this one.
$sinks = glob($dir . '/crash-*.bin') ?: [];
var_dump(count($sinks) === 2);

$records = cbox_telemetry_drain_crashes();
var_dump(count($records) === 2);
var_dump($records[0]['signal_name']);
var_dump($records[0]['trace_id']);

// Consumed sinks are removed, and nothing is left behind.
var_dump(glob($dir . '/crash-*.bin') ?: []);

// Nothing left to find.
var_dump(cbox_telemetry_drain_crashes());

foreach (glob($dir . '/*') ?: [] as $leftover) {
    if ($leftover !== $mine) {
        @unlink($leftover);
    }
}
?>
--EXPECT--
bool(true)
bool(false)
bool(true)
bool(true)
string(7) "SIGABRT"
string(32) "4bf92f3577b34da6a3ce929d0e0e4736"
array(0) {
}
array(0) {
}
