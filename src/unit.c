#include "unit.h"

#include <string.h>

static const char cbox_hex_digits[] = "0123456789abcdef";

const char *cbox_unit_type_name(cbox_unit_type type)
{
	switch (type) {
		case CBOX_UNIT_HTTP:     return "http";
		case CBOX_UNIT_QUEUE:    return "queue";
		case CBOX_UNIT_COMMAND:  return "command";
		case CBOX_UNIT_SCHEDULE: return "schedule";
		case CBOX_UNIT_OTHER:    return "other";
		case CBOX_UNIT_NONE:
		case CBOX_UNIT_MAX:
		default:                 return "none";
	}
}

cbox_unit_type cbox_unit_type_from(const char *name, size_t len)
{
	if (name == NULL) {
		return CBOX_UNIT_OTHER;
	}

	if (len == 4 && memcmp(name, "http", 4) == 0) {
		return CBOX_UNIT_HTTP;
	}

	if (len == 5 && memcmp(name, "queue", 5) == 0) {
		return CBOX_UNIT_QUEUE;
	}

	if (len == 7 && memcmp(name, "command", 7) == 0) {
		return CBOX_UNIT_COMMAND;
	}

	if (len == 8 && memcmp(name, "schedule", 8) == 0) {
		return CBOX_UNIT_SCHEDULE;
	}

	return CBOX_UNIT_OTHER;
}

void cbox_unit_reset(cbox_unit_state *unit)
{
	memset(unit, 0, sizeof(*unit));
}

static inline int cbox_hex_value(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}

	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}

	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}

	return -1;
}

bool cbox_hex_decode(const char *hex, size_t len, uint8_t *out, size_t out_len)
{
	size_t i;

	if (hex == NULL || len != out_len * 2) {
		return false;
	}

	for (i = 0; i < out_len; i++) {
		int hi = cbox_hex_value(hex[i * 2]);
		int lo = cbox_hex_value(hex[i * 2 + 1]);

		if (hi < 0 || lo < 0) {
			return false;
		}

		out[i] = (uint8_t) ((hi << 4) | lo);
	}

	return true;
}

void cbox_hex_encode(const uint8_t *bytes, size_t len, char *out)
{
	size_t i;

	for (i = 0; i < len; i++) {
		out[i * 2] = cbox_hex_digits[bytes[i] >> 4];
		out[i * 2 + 1] = cbox_hex_digits[bytes[i] & 0x0f];
	}
}

bool cbox_bytes_are_zero(const uint8_t *bytes, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (bytes[i] != 0) {
			return false;
		}
	}

	return true;
}
