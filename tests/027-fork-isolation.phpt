--TEST--
cbox_telemetry: a forked child never reports the parent's unit as its own
--EXTENSIONS--
cbox_telemetry
--SKIPIF--
<?php if (!extension_loaded('pcntl')) die('skip ext-pcntl not available'); ?>
--INI--
cbox_telemetry.profiler.allow_fallback_backend=1
cbox_telemetry.profiler.period_us=500
--FILE--
<?php
/*
 * finish() and status() used to skip fork detection, so a child following the
 * documented terminate-hook pattern — straight to finish(0) — collected the
 * parent's inherited samples, and the parent then reported the same ones again.
 */
function cbox_spin(float $ms): void
{
    $until = microtime(true) + $ms / 1000;

    while (microtime(true) < $until) {
        for ($i = 0; $i < 5000; $i++) {
            sqrt($i);
        }
    }
}

$parent = cbox_telemetry_begin(['unit' => 'http', 'profile' => true]);
cbox_spin(150);

$pid = pcntl_fork();

if ($pid === 0) {
    // Nothing of the parent's may be visible here.
    $status = cbox_telemetry_status();
    $inherited = $status['unit_handle'] !== 0 || $status['profiler_running'];
    $result = cbox_telemetry_finish(0, true);

    exit($inherited === false && $result === [] ? 0 : 1);
}

pcntl_waitpid($pid, $status);
var_dump(pcntl_wexitstatus($status) === 0);

// The parent still has its own, intact.
$result = cbox_telemetry_finish($parent, true);
var_dump($result['profile']['sample_count'] > 0);
?>
--EXPECT--
bool(true)
bool(true)
