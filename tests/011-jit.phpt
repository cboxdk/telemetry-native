--TEST--
cbox_telemetry: frames stay resolvable with the tracing JIT enabled
--EXTENSIONS--
cbox_telemetry
--SKIPIF--
<?php
if (!extension_loaded('Zend OPcache')) die('skip opcache not available');
if (!function_exists('opcache_get_status')) die('skip opcache_get_status unavailable');
?>
--INI--
cbox_telemetry.profiler.period_us=200
opcache.enable=1
opcache.enable_cli=1
opcache.jit=tracing
opcache.jit_buffer_size=16M
--FILE--
<?php
function cbox_jit_hot(int $n): float
{
    $total = 0.0;

    for ($i = 0; $i < $n; $i++) {
        $total += sqrt($i) + log($i + 1);
    }

    return $total;
}

$handle = cbox_telemetry_begin(['profile' => true]);

// Enough work that even a coarse wall-clock backend collects samples.
for ($i = 0; $i < 400; $i++) {
    cbox_jit_hot(20000);
}

$profile = cbox_telemetry_finish($handle, true)['profile'];
$functions = array_column($profile['frames'], 'function');

var_dump($profile['sample_count'] > 0);
var_dump(in_array('cbox_jit_hot', $functions, true));

// Declaration lines are read from the op_array, not from a possibly stale
// opline, so they stay sane under JIT.
foreach ($profile['frames'] as $frame) {
    if ($frame['file'] !== null && $frame['line'] < 0) {
        echo "bad line\n";
    }
}

echo "done\n";
?>
--EXPECT--
bool(true)
bool(true)
done
