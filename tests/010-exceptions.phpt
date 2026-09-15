--TEST--
cbox_telemetry: unwinding through exceptions leaves no stale state
--EXTENSIONS--
cbox_telemetry
--INI--
cbox_telemetry.profiler.period_us=200
--FILE--
<?php
function cbox_throwing(int $depth): void
{
    for ($i = 0; $i < 20000; $i++) {
        sqrt($i);
    }

    if ($depth > 0) {
        cbox_throwing($depth - 1);
    }

    throw new RuntimeException('boom');
}

$handle = cbox_telemetry_begin(['profile' => true]);

for ($i = 0; $i < 50; $i++) {
    try {
        cbox_throwing(5);
    } catch (RuntimeException) {
        // deliberate
    }
}

$result = cbox_telemetry_finish($handle, true, true);

var_dump($result['profile']['sample_count'] > 0);
var_dump($result['profile']['dropped'] === 0);

$status = cbox_telemetry_status();
var_dump($status['profiler_running'], $status['unit_handle']);
?>
--EXPECT--
bool(true)
bool(true)
bool(false)
int(0)
