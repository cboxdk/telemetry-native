--TEST--
cbox_telemetry: API defects found in the pre-release review stay fixed
--EXTENSIONS--
cbox_telemetry
--INI--
cbox_telemetry.profiler.allow_fallback_backend=1
cbox_telemetry.profiler.period_us=500
--FILE--
<?php
// The stub declares `array $context = []`; ZPP used to accept null, which a
// debug build rejects outright with "Arginfo / zpp mismatch".
try {
    cbox_telemetry_begin(null);
    echo "null was accepted\n";
} catch (TypeError) {
    echo "null rejected\n";
}

// Context values passed by reference were silently ignored.
$trace = '4bf92f3577b34da6a3ce929d0e0e4736';
$context = ['unit' => 'http', 'trace_id' => &$trace];
$handle = cbox_telemetry_begin($context);
var_dump(cbox_telemetry_finish($handle)['unit']);

// Truthiness must never make the caller's code emit a diagnostic.
$handle = cbox_telemetry_begin(['sampled' => NAN, 'profile' => NAN]);
cbox_telemetry_finish($handle);
echo "no diagnostics\n";

// period_us and max_depth are PHP_INI_ALL but clamped at startup, so status
// used to report values the profiler would never use.
ini_set('cbox_telemetry.profiler.period_us', '-5');
ini_set('cbox_telemetry.profiler.max_depth', '99999');
$limits = cbox_telemetry_status()['limits'];
var_dump($limits['period_us'], $limits['max_depth']);

// These span the process, and used to be named as though they were per-unit.
$handle = cbox_telemetry_begin();
$counters = cbox_telemetry_finish($handle)['counters'];
var_dump(
    array_key_exists('breadcrumbs.written_total', $counters),
    array_key_exists('arena.peak_bytes_total', $counters)
);

// An unusable span id must be reported as absent, not as all zeros.
$handle = cbox_telemetry_begin(['trace_id' => $trace, 'span_id' => 'nonsense']);
cbox_telemetry_finish($handle);
echo "done\n";
?>
--EXPECT--
null rejected
string(4) "http"
no diagnostics
int(100)
int(256)
bool(true)
bool(true)
done
