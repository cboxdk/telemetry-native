--TEST--
cbox_telemetry: state does not bleed between units in a long-lived worker
--EXTENSIONS--
cbox_telemetry
--INI--
cbox_telemetry.profiler.period_us=200
--FILE--
<?php
function cbox_unique_work(int $n): float
{
    $total = 0.0;

    for ($i = 0; $i < 20000; $i++) {
        $total += sqrt($i + $n);
    }

    return $total;
}

$peak = 0;

for ($unit = 0; $unit < 200; $unit++) {
    $handle = cbox_telemetry_begin(['unit' => 'queue', 'profile' => true]);
    cbox_unique_work($unit);
    $result = cbox_telemetry_finish($handle, true);

    // Each unit reports only its own samples, never an accumulating total.
    if ($result['profile'] !== null && $result['profile']['sample_count'] > 5000) {
        echo "sample count accumulated across units\n";
    }

    $peak = max($peak, $result['counters']['arena.peak_bytes']);
}

$status = cbox_telemetry_status();

// Nothing is left running or allocated once the last unit is finished.
var_dump($status['unit_handle'], $status['profiler_running'], $status['profiler_samples']);
var_dump($status['profiler_dropped'] === 0);

// The arena never grew with the number of units.
var_dump($peak < 65536);
echo "done\n";
?>
--EXPECT--
int(0)
bool(false)
int(0)
bool(true)
bool(true)
done
