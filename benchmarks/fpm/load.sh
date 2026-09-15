#!/usr/bin/env bash
#
# Latency and memory under concurrent PHP-FPM load, on the production image.
#
# Everything else in this repo measures one process doing one thing at a time.
# This measures what actually ships: several workers, each with its own timer,
# competing for the same cores.
#
# Usage: benchmarks/fpm/load.sh [requests] [concurrency] [php-version]
set -euo pipefail

REQUESTS="${1:-4000}"
CONCURRENCY="${2:-8}"
PHP_VERSION="${3:-8.4}"
REPEATS="${4:-3}"
SOAK_SECONDS="${5:-0}"
WORKERS=${WORKERS:-8}

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
NET=cbox-load-net
# Bind mounts have to come from a path Docker Desktop shares; mktemp's default
# on macOS (/var/folders/...) is not one of them.
WORKDIR=/tmp/cbox-load-$$
mkdir -p "$WORKDIR"
APP=cbox-load-app
IMAGE=cbox-telemetry-load:$PHP_VERSION

cleanup() {
  docker rm -f "$APP" >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
  rm -rf "$WORKDIR"
}
trap cleanup EXIT

echo "building the load image on the production base (php $PHP_VERSION)…"
STAGE="$WORKDIR/stage"
mkdir -p "$STAGE"
cp "$ROOT"/config.m4 "$ROOT"/cbox_telemetry.c "$ROOT"/cbox_telemetry_arginfo.h \
   "$ROOT"/cbox_telemetry.stub.php "$ROOT"/php_cbox_telemetry.h "$STAGE/"
mkdir -p "$STAGE/src"
cp "$ROOT"/src/*.c "$ROOT"/src/*.h "$STAGE/src/"
cp "$HERE/www-work.php" "$HERE/Dockerfile" "$STAGE/"
docker build -q --build-arg "PHP_VERSION=$PHP_VERSION" -t "$IMAGE" "$STAGE" >/dev/null
rm -rf "$STAGE"

docker network create "$NET" >/dev/null 2>&1 || true

# ab comes from the httpd image (multi-arch); the usual standalone ab images
# have no arm64 build.
AB_IMAGE=cbox-load-ab
docker build -q -t "$AB_IMAGE" - <<'AB' >/dev/null
FROM httpd:2.4-alpine
ENV PATH="/usr/local/apache2/bin:${PATH}"
AB

# Scenarios are interleaved, not run one after another.
#
# Running them in sequence made the extension look 8.5% more expensive than
# baseline purely by being measured later: an A/B/A/B of the same two configs
# came back at -0.3%, +0.8%, -0.8%. The machine drifts over minutes, so every
# scenario has to be sampled in every round and compared by median.
start_app() {
  local ini_file="$1"

  docker rm -f "$APP" >/dev/null 2>&1 || true
  docker run -d --name "$APP" --network "$NET" \
    --cpus=4 \
    -e PHP_FPM_PM=static \
    -e PHP_FPM_MAX_CHILDREN=$WORKERS \
    -e PHP_FPM_MAX_REQUESTS=0 \
    -v "$ini_file:/usr/local/etc/php/conf.d/99-cbox.ini:ro" \
    "$IMAGE" >/dev/null

  for _ in $(seq 1 60); do
    if docker run --rm --network "$NET" curlimages/curl:8.11.1 -sf "http://$APP/" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.5
  done

  echo "  container never became ready"
  docker logs "$APP" 2>&1 | tail -5
  return 1
}

# CPU consumed by the whole container, from the cgroup.
#
# Two earlier attempts were wrong and worth recording. Requests per second
# through nginx, FPM, a socket and two container layers is noisy enough to rank
# "loaded but never called" as slower than "loaded and hooking", which cannot
# be true. Summing per-worker CPU from /proc is worse: FPM recycles workers, so
# the total goes *down* mid-measurement and yields negative overhead.
#
# The cgroup counter is monotonic, survives worker recycling, and includes
# nginx and the supervisor — identical across scenarios, so the difference
# between them is still exactly the extension's cost.
container_cpu_usec() {
  docker exec "$APP" sh -c "awk '/usage_usec/{print \$2}' /sys/fs/cgroup/cpu.stat" 2>/dev/null || echo 0
}

worker_rss() {
  docker exec "$APP" sh -c \
    "ps -o rss= -C php-fpm | sort -n | tail -n $WORKERS | awk '{s+=\$1; n++} END {if (n) printf \"%d\", s/n; else print 0}'" \
    2>/dev/null || echo 0
}

median() {
  # shellcheck disable=SC2086
  printf '%s\n' $1 | sort -n | awk '{v[NR]=$1} END {print v[int((NR+1)/2)]}'
}

SCENARIO_LABELS=(
  "baseline (no extension)"
  "loaded, never called"
  "hooks armed"
  "auto profiling @1ms"
  "auto profiling @200us"
)

SCENARIO_INIS=(
  ""
  "extension=cbox_telemetry.so
cbox_telemetry.auto=0
cbox_telemetry.crash.enabled=0"
  "extension=cbox_telemetry.so
cbox_telemetry.auto=0
cbox_telemetry.crash.enabled=1"
  "extension=cbox_telemetry.so
cbox_telemetry.auto=1
cbox_telemetry.profiler.period_us=1000
cbox_telemetry.crash.enabled=1"
  "extension=cbox_telemetry.so
cbox_telemetry.auto=1
cbox_telemetry.profiler.period_us=200
cbox_telemetry.crash.enabled=1"
)

measure_once() {
  local index="$1"
  local ini_file="$WORKDIR/scenario-$index.ini"

  printf '%s\n' "${SCENARIO_INIS[$index]}" > "$ini_file"
  start_app "$ini_file" || return 1

  # Warm opcache and let the pool settle before anything is measured.
  docker run --rm --network "$NET" "$AB_IMAGE" \
    ab -q -c "$CONCURRENCY" -n 600 "http://$APP/" >/dev/null 2>&1 || true

  local before after out
  before=$(container_cpu_usec)
  out=$(docker run --rm --network "$NET" "$AB_IMAGE" \
    ab -q -c "$CONCURRENCY" -n "$REQUESTS" "http://$APP/" 2>/dev/null)
  after=$(container_cpu_usec)

  MEASURED_CPU=$(awk -v a="$after" -v b="$before" -v n="$REQUESTS" \
    'BEGIN { printf "%.3f", (a - b) / 1000.0 / n }')
  MEASURED_RPS=$(echo "$out" | awk '/Requests per second/ {printf "%d", $4}')
  MEASURED_P95=$(echo "$out" | awk '/^  95%/ {print $2}')
  MEASURED_RSS=$(worker_rss)
}

echo
echo "php $PHP_VERSION · $WORKERS workers · concurrency $CONCURRENCY · $REQUESTS requests"
echo "$REPEATS interleaved rounds, medians. CPU from the container cgroup."
echo

declare -a CPU_SAMPLES RPS_SAMPLES P95_SAMPLES RSS_LAST
for i in "${!SCENARIO_LABELS[@]}"; do
  CPU_SAMPLES[$i]=""; RPS_SAMPLES[$i]=""; P95_SAMPLES[$i]=""; RSS_LAST[$i]=0
done

for round in $(seq 1 "$REPEATS"); do
  echo "  round $round/$REPEATS…"
  for i in "${!SCENARIO_LABELS[@]}"; do
    measure_once "$i" || continue
    CPU_SAMPLES[$i]="${CPU_SAMPLES[$i]} $MEASURED_CPU"
    RPS_SAMPLES[$i]="${RPS_SAMPLES[$i]} $MEASURED_RPS"
    P95_SAMPLES[$i]="${P95_SAMPLES[$i]} $MEASURED_P95"
    RSS_LAST[$i]="$MEASURED_RSS"
  done
done

echo
BASE_CPU=$(median "${CPU_SAMPLES[0]}")

for i in "${!SCENARIO_LABELS[@]}"; do
  cpu=$(median "${CPU_SAMPLES[$i]}")
  delta=$(awk -v c="$cpu" -v b="$BASE_CPU" 'BEGIN { if (b > 0) printf "%+.1f%%", (c - b) / b * 100; else print "n/a" }')
  [ "$i" = 0 ] && delta="—"
  printf '  %-26s %7s CPU ms/req  %8s   %5s rps   p95 %3s ms   RSS %6s KB\n' \
    "${SCENARIO_LABELS[$i]}" "$cpu" "$delta" \
    "$(median "${RPS_SAMPLES[$i]}")" "$(median "${P95_SAMPLES[$i]}")" "${RSS_LAST[$i]}"
done

if [ "$SOAK_SECONDS" -gt 0 ]; then
  printf '%s\n' "${SCENARIO_INIS[3]}" > "$WORKDIR/soak.ini"
  start_app "$WORKDIR/soak.ini" || exit 1

  echo
  echo "soak: ${SCENARIO_LABELS[3]}, ${SOAK_SECONDS}s sustained at concurrency $CONCURRENCY"

  docker run -d --rm --name cbox-load-soak --network "$NET" "$AB_IMAGE" \
    ab -q -c "$CONCURRENCY" -t "$SOAK_SECONDS" -n 100000000 "http://$APP/" >/dev/null 2>&1

  elapsed=0; first=""
  while [ "$elapsed" -lt "$SOAK_SECONDS" ]; do
    sleep 15
    elapsed=$((elapsed + 15))
    rss=$(worker_rss)
    [ -z "$first" ] && first="$rss"
    printf '  %4ss  avg worker RSS %6s KB  (%+d KB)\n' "$elapsed" "$rss" "$((rss - first))"
  done

  docker rm -f cbox-load-soak >/dev/null 2>&1 || true
fi
