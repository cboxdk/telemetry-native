--TEST--
cbox_telemetry: begin() adopts the automatic unit instead of discarding the bootstrap
--EXTENSIONS--
cbox_telemetry
--INI--
cbox_telemetry.profiler.allow_fallback_backend=1
cbox_telemetry.auto=1
cbox_telemetry.profiler.period_us=200
--FILE--
<?php
/* Time-bounded for the same reason as 017: see the note there. */
function cbox_spin(float $ms): void
{
    $until = microtime(true) + $ms / 1000;

    while (microtime(true) < $until) {
        for ($i = 0; $i < 5000; $i++) {
            sqrt($i);
        }
    }
}

function cbox_before(): void
{
    cbox_spin(120);
}

function cbox_after(): void
{
    cbox_spin(120);
}

$automatic = cbox_telemetry_status()['unit_handle'];

// Work that happens before the framework knows its trace context.
cbox_before();

$handle = cbox_telemetry_begin([
    'unit'     => 'http',
    'trace_id' => '4bf92f3577b34da6a3ce929d0e0e4736',
    'span_id'  => '00f067aa0ba902b7',
]);

// Adoption, not replacement: same unit.
var_dump($handle === $automatic);

cbox_after();

$result = cbox_telemetry_finish($handle, true, true);
$profile = $result['profile'];

/*
 * Ask the call tree, not the leaf. Which frame a sample lands on for an
 * internal call is a version-dependent detail — PHP 8.3 credits the caller,
 * 8.4 credits sqrt() itself — and the thing under test here is neither. What
 * matters is that work from *both* sides of the begin() is in one profile.
 */
$frameId = [];

foreach ($profile['frames'] as $id => $frame) {
    $frameId[$frame['function']] = $id;
}

$nodes = [];

foreach ($profile['stacks'] as $index => [$parent, $frame, $samples]) {
    $nodes[$index + 1] = ['parent' => $parent, 'frame' => $frame, 'samples' => $samples];
}

$inclusive = static function (string $function) use ($nodes, $frameId): int {
    $wanted = $frameId[$function] ?? -1;
    $total = 0;

    foreach ($nodes as $node) {
        // Walk to the root; count the node's samples if $function is on its path.
        for ($at = $node; $at !== null; $at = $nodes[$at['parent']] ?? null) {
            if ($at['frame'] === $wanted) {
                $total += $node['samples'];
                break;
            }
        }
    }

    return $total;
};

var_dump($inclusive('cbox_before') > 0);
var_dump($inclusive('cbox_after') > 0);

// The caller's label wins over the SAPI guess.
var_dump($result['unit']);
var_dump($result['automatic']);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
string(4) "http"
bool(true)
