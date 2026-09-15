# The real Cbox production runtime, not a stock php image: same PHP build, same
# nginx, same pool defaults. Overhead measured anywhere else is overhead for a
# machine nobody runs.
ARG PHP_VERSION=8.4
FROM ghcr.io/cboxdk/php-baseimages/php-fpm-nginx:${PHP_VERSION}-bookworm-v1

COPY src/ /build/src/
COPY config.m4 cbox_telemetry.c cbox_telemetry_arginfo.h cbox_telemetry.stub.php php_cbox_telemetry.h /build/

RUN set -eux; \
    cd /build; \
    phpize; \
    ./configure --enable-cbox-telemetry; \
    make -j"$(nproc)"; \
    make install; \
    php -d extension=cbox_telemetry.so -r 'echo cbox_telemetry_version(), PHP_EOL;'

# The ini is mounted per scenario, so one image serves every configuration.
COPY www-work.php /var/www/html/public/index.php
