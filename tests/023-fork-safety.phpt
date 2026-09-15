--TEST--
cbox_telemetry: a forked child rebuilds its own timer instead of inheriting a lie
--EXTENSIONS--
cbox_telemetry
--SKIPIF--
<?php if (!extension_loaded('pcntl')) die('skip ext-pcntl not available'); ?>
--INI--
cbox_telemetry.profiler.allow_fallback_backend=1
cbox_telemetry.profiler.period_us=200
--FILE--
<?php
/*
 * fork() copies this extension's memory but not its timer: POSIX per-thread
 * timers are explicitly not inherited, and neither are threads. A child that
 * trusts the copied state reports profiling as active and samples nothing.
 */
function cbox_fork_work(float $ms): void
{
    $until = microtime(true) + $ms / 1000;

    while (microtime(true) < $until) {
        for ($i = 0; $i < 5000; $i++) {
            sqrt($i);
        }
    }
}

$parent = cbox_telemetry_begin(['profile' => true]);
cbox_fork_work(80);

$pid = pcntl_fork();

if ($pid === 0) {
    $child = cbox_telemetry_begin(['unit' => 'queue', 'profile' => true]);
    cbox_fork_work(150);
    $result = cbox_telemetry_finish($child, true);

    // 0 only if the child actually collected something.
    exit(($result['profile']['sample_count'] ?? 0) > 0 ? 0 : 1);
}

pcntl_waitpid($pid, $status);
var_dump(pcntl_wexitstatus($status) === 0);

// The parent keeps its own profile, including what it sampled before the fork.
$result = cbox_telemetry_finish($parent, true);
var_dump($result['profile']['sample_count'] > 0);
?>
--EXPECT--
bool(true)
bool(true)
