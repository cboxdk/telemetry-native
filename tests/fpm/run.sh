#!/bin/sh
# Crash-recorder lifecycle under real PHP-FPM privilege separation.
#
# This is the case unit tests cannot reach: the FPM master starts as root and
# runs MINIT, then forks workers that drop to another user. Anything the
# recorder creates during startup belongs to root, and a worker that cannot
# write or reopen it has a recorder that silently does nothing.
#
# Run inside a php:*-fpm image: sh tests/fpm/run.sh
set -e

fail() { echo "FAIL: $*"; exit 1; }
ok()   { echo "ok: $*"; }

apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq $PHPIZE_DEPS libfcgi-bin procps >/dev/null 2>&1

cd /src
rm -rf /build && mkdir -p /build/src && cd /build
cp /src/config.m4 /src/cbox_telemetry.c /src/cbox_telemetry_arginfo.h /src/cbox_telemetry.stub.php /src/php_cbox_telemetry.h .
cp /src/src/*.c /src/src/*.h src/
phpize >/dev/null 2>&1
./configure --enable-cbox-telemetry >/dev/null 2>&1
make -j4 >/dev/null 2>&1
test -f modules/cbox_telemetry.so || fail "extension did not build"

CRASH_DIR=/tmp/cbox-telemetry-fpm
rm -rf "$CRASH_DIR"

cat > /usr/local/etc/php/conf.d/99-cbox.ini <<INI
extension=/build/modules/cbox_telemetry.so
cbox_telemetry.crash.enabled=1
cbox_telemetry.crash.dir=$CRASH_DIR
cbox_telemetry.auto=0
INI

# Master as root, workers as www-data: the split this test exists for.
rm -f /usr/local/etc/php-fpm.d/*.conf
cat > /usr/local/etc/php-fpm.d/www.conf <<CONF
[www]
user = www-data
group = www-data
listen = 127.0.0.1:9000
pm = static
pm.max_children = 2
catch_workers_output = yes
CONF

mkdir -p /www
cat > /www/status.php <<'PHP'
<?php
$s = cbox_telemetry_status();
echo json_encode([
    'pid' => getmypid(),
    'uid' => function_exists('posix_geteuid') ? posix_geteuid() : -1,
    'recorder' => $s['crash_recorder'],
    'path' => $s['crash_path'],
]), "\n";
PHP
cat > /www/drain.php <<'PHP'
<?php
$records = cbox_telemetry_drain_crashes(10);
echo json_encode([
    'count' => count($records),
    'signal' => $records[0]['signal_name'] ?? null,
    'unit' => $records[0]['unit'] ?? null,
]), "\n";
PHP

php-fpm --daemonize --fpm-config /usr/local/etc/php-fpm.conf 2>&1 | tail -5
sleep 2
pgrep -f "php-fpm: master" >/dev/null || fail "php-fpm master did not start"
ok "php-fpm running (master as $(ps -o user= -p "$(pgrep -f 'php-fpm: master' | head -1)"))"

request() {
  SCRIPT_FILENAME="$1" REQUEST_METHOD=GET SCRIPT_NAME="$1" \
    cgi-fcgi -bind -connect 127.0.0.1:9000 2>/dev/null | tail -1
}

FIRST=$(request /www/status.php)
echo "  worker says: $FIRST"

echo "$FIRST" | grep -q '"recorder":"armed"' \
  || fail "recorder not armed in worker: $FIRST"
ok "recorder armed inside a non-root worker"

test -d "$CRASH_DIR" || fail "crash directory was never created"
DIR_OWNER=$(stat -c '%U' "$CRASH_DIR")
[ "$DIR_OWNER" = "www-data" ] \
  || fail "crash dir owned by $DIR_OWNER, not the worker user — a root-owned dir is exactly the bug this guards"
ok "crash dir created by the worker, owned by $DIR_OWNER"

WORKER_PID=$(echo "$FIRST" | sed 's/.*"pid":\([0-9]*\).*/\1/')
# json_encode escapes forward slashes.
SINK=$(echo "$FIRST" | sed 's/.*"path":"\([^"]*\)".*/\1/' | sed 's|\\/|/|g')
[ -f "$SINK" ] || fail "worker sink $SINK does not exist"
SINK_OWNER=$(stat -c '%U' "$SINK")
[ "$SINK_OWNER" = "www-data" ] || fail "sink owned by $SINK_OWNER"
ok "worker $WORKER_PID has its own sink, owned by $SINK_OWNER"

# Kill that worker the way a native segfault would.
kill -ABRT "$WORKER_PID" 2>/dev/null || fail "could not signal worker $WORKER_PID"
sleep 2

[ -f "$SINK" ] || fail "no crash record left behind by the dead worker"
ok "dead worker left a record ($(stat -c '%s' "$SINK") bytes)"

# FPM replaces the worker; a *different* worker must be able to drain it.
DRAINED=$(request /www/drain.php)
echo "  drain says: $DRAINED"
echo "$DRAINED" | grep -q '"count":1' || fail "a live worker could not drain the dead one's record: $DRAINED"
echo "$DRAINED" | grep -q '"signal":"SIGABRT"' || fail "wrong signal recorded: $DRAINED"
ok "a replacement worker drained the dead worker's record"

AGAIN=$(request /www/drain.php)
echo "$AGAIN" | grep -q '"count":0' || fail "record was reported twice: $AGAIN"
ok "draining consumes: the record is not reported again"

# Live workers' sinks must survive the drain, or the next crash goes nowhere.
LIVE=$(request /www/status.php)
LIVE_SINK=$(echo "$LIVE" | sed 's/.*"path":"\([^"]*\)".*/\1/' | sed 's|\\/|/|g')
[ -f "$LIVE_SINK" ] || fail "a live worker's sink was removed by the drain"
ok "live workers keep their sinks"

echo
echo "ALL FPM CHECKS PASSED"
