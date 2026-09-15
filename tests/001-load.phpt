--TEST--
cbox_telemetry: loads and reports a coherent status
--EXTENSIONS--
cbox_telemetry
--INI--
cbox_telemetry.profiler.allow_fallback_backend=1
--FILE--
<?php
$status = cbox_telemetry_status();

var_dump(cbox_telemetry_version() === $status['version']);
var_dump($status['enabled']);
var_dump(in_array($status['timer_backend'], ['posix-thread-cputime', 'thread-walltime'], true));

// Only the CPU-time backend may claim CPU-time sampling.
var_dump($status['timer_cpu_time'] === ($status['timer_backend'] === 'posix-thread-cputime'));

// The profiler must never share a signal with PHP's own execution timer;
// SIGPROF is what max_execution_time uses without ZEND_MAX_EXECUTION_TIMERS.
// (27 on Linux and macOS; pcntl is not always loaded, so do not rely on the constant.)
var_dump($status['timer_signal'] !== 27);

var_dump($status['limits']['period_us'] >= 100 && $status['limits']['period_us'] <= 100000);
var_dump($status['limits']['max_depth'] >= 1 && $status['limits']['max_depth'] <= 256);
var_dump($status['unit_handle']);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
int(0)
