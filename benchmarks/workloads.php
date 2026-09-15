<?php

/**
 * Benchmark workloads. Each returns a callable that does one unit of work,
 * shaped to stress a different part of the extension:
 *
 *   cpu       tight arithmetic — few frames, many samples
 *   calls     millions of shallow userland calls — the worst case for any
 *             per-call instrumentation, and what the observer decision hinges on
 *   deep      recursion — stack walks cost O(depth) per sample
 *   internal  heavy internal-function use — what the hooks have to stay out of
 *   mixed     something closer to a real request
 */

declare(strict_types=1);

function cbox_bench_leaf(int $n): float
{
    return sqrt((float) $n) + 1.0;
}

function cbox_bench_mid(int $n): float
{
    return cbox_bench_leaf($n) + cbox_bench_leaf($n + 1);
}

function cbox_bench_recurse(int $depth, int $n): float
{
    if ($depth <= 0) {
        return cbox_bench_leaf($n);
    }

    return cbox_bench_recurse($depth - 1, $n);
}

/** @return array<string, callable(): void> */
function cbox_bench_workloads(): array
{
    return [
        'cpu' => static function (): void {
            $total = 0.0;
            for ($i = 1; $i < 400000; $i++) {
                $total += sqrt($i) * log($i);
            }
        },

        'calls' => static function (): void {
            $total = 0.0;
            for ($i = 0; $i < 300000; $i++) {
                $total += cbox_bench_mid($i);
            }
        },

        'deep' => static function (): void {
            $total = 0.0;
            for ($i = 0; $i < 20000; $i++) {
                $total += cbox_bench_recurse(40, $i);
            }
        },

        'internal' => static function (): void {
            $parts = [];
            for ($i = 0; $i < 60000; $i++) {
                $parts[] = str_pad((string) $i, 12, '0', STR_PAD_LEFT);
            }
            $joined = implode(',', $parts);
            $back = explode(',', $joined);
            usort($back, static fn (string $a, string $b): int => strcmp($a, $b));
        },

        'mixed' => static function (): void {
            $rows = [];
            for ($i = 0; $i < 30000; $i++) {
                $rows[] = ['id' => $i, 'name' => 'row-' . $i, 'score' => cbox_bench_mid($i)];
            }
            usort($rows, static fn (array $a, array $b): int => $a['score'] <=> $b['score']);
            $total = 0.0;
            foreach ($rows as $row) {
                $total += $row['score'];
            }
        },
    ];
}
