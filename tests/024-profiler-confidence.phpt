--TEST--
cbox_telemetry: a profile says how much of it was sampled late
--EXTENSIONS--
cbox_telemetry
--INI--
cbox_telemetry.profiler.allow_fallback_backend=1
cbox_telemetry.profiler.period_us=1000
--FILE--
<?php
/*
 * Two different things make a sample land late, and they mean opposite things:
 *
 *   deferred        the VM had not reached a safe point since the last tick,
 *                   because PHP was inside a long internal call
 *   timer_overruns  the kernel could not deliver at the requested period at all
 *
 * One is a fact about the application, the other about the configuration.
 */
function cbox_pure_php(float $ms): void
{
    $until = microtime(true) + $ms / 1000;

    while (microtime(true) < $until) {
        for ($i = 0; $i < 5000; $i++) {
            sqrt($i);
        }
    }
}

$handle = cbox_telemetry_begin(['profile' => true]);
cbox_pure_php(250);
$result = cbox_telemetry_finish($handle, true);
$profile = $result['profile'];

// Reported on the profile and in the counters, and they agree.
var_dump($profile['deferred_samples'] === $result['counters']['profiler.deferred_samples']);
var_dump($profile['timer_overruns'] === $result['counters']['profiler.timer_overruns']);
var_dump(array_key_exists('max_deferred', $profile));

// Deferred samples are a subset of the samples taken, never more.
var_dump($profile['deferred_samples'] <= $profile['sample_count']);
var_dump($profile['deferred_events'] <= $profile['sample_count']);
var_dump($profile['max_deferred'] <= $profile['sample_count']);

/*
 * On the CPU-time backend at the default period, a tight PHP loop reaches
 * safe points constantly, so essentially nothing should be deferred. The
 * wall-clock fallback keeps counting while the process is descheduled and
 * cannot make that promise, so it is only asserted where it means something.
 */
if (cbox_telemetry_status()['timer_cpu_time']) {
    $direct = $profile['sample_count'] - $profile['deferred_samples'];
    var_dump($direct / max(1, $profile['sample_count']) > 0.9);
} else {
    var_dump(true);
}

// Counting starts over with each unit.
$handle = cbox_telemetry_begin(['profile' => true]);
cbox_pure_php(20);
$counters = cbox_telemetry_finish($handle)['counters'];
var_dump($counters['profiler.deferred_samples'] <= $counters['profiler.samples']);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
