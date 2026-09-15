#include "php.h"

#include "php_cbox_telemetry.h"
#include "hooks.h"

#include <string.h>

/*
 * Each hook replaces one internal function's handler with a wrapper that
 * brackets the original call. The engine calls the wrapper exactly where it
 * would have called the original, so there is no dispatch cost anywhere else.
 */
typedef struct _cbox_hook {
	const char      *class_name; /* NULL for a plain function */
	const char      *function_name;
	cbox_op_type     op;
	zif_handler      original;
	zend_function   *target;
	bool             installed;
} cbox_hook;

#define CBOX_HOOK_MAX 8

static cbox_hook cbox_hooks[CBOX_HOOK_MAX];
static uint32_t  cbox_hook_count = 0;
static bool      cbox_hooks_done = false;
static char      cbox_hook_summary[128];

enum { CBOX_GROUP_PDO = 0, CBOX_GROUP_REDIS, CBOX_GROUP_CURL, CBOX_GROUP_STREAMS, CBOX_GROUP_MAX };

static cbox_hook_group cbox_hook_groups[CBOX_GROUP_MAX] = {
	{"pdo", false, 0, 0},
	{"redis", false, 0, 0},
	{"curl", false, 0, 0},
	{"streams", false, 0, 0},
};

/* "PDO::__construct", "curl_exec", … in installation order. */
static char     cbox_hook_names[CBOX_HOOK_MAX][64];
static uint32_t cbox_hook_names_count = 0;

/*
 * A zend_bailout (fatal error, timeout, exit) longjmps straight past the end
 * of the wrapper, so an operation that dies mid-call never records its end.
 * That costs one measurement, never correctness: the nesting stack is bounded
 * and is reset at every unit boundary and at RSHUTDOWN. Wrapping each call in
 * zend_try would close the gap, but a setjmp per curl_exec is a worse trade
 * than losing the timing of a call that killed the request anyway.
 */
#define CBOX_DEFINE_HOOK_HANDLER(index)                                        \
	static ZEND_NAMED_FUNCTION(cbox_hook_handler_##index)                      \
	{                                                                          \
		cbox_hook *hook = &cbox_hooks[index];                                  \
                                                                               \
		cbox_telemetry_note_op_begin(hook->op);                                \
		hook->original(INTERNAL_FUNCTION_PARAM_PASSTHRU);                      \
		cbox_telemetry_note_op_end(hook->op);                                  \
	}

CBOX_DEFINE_HOOK_HANDLER(0)
CBOX_DEFINE_HOOK_HANDLER(1)
CBOX_DEFINE_HOOK_HANDLER(2)
CBOX_DEFINE_HOOK_HANDLER(3)
CBOX_DEFINE_HOOK_HANDLER(4)
CBOX_DEFINE_HOOK_HANDLER(5)
CBOX_DEFINE_HOOK_HANDLER(6)
CBOX_DEFINE_HOOK_HANDLER(7)

static const zif_handler cbox_hook_handlers[CBOX_HOOK_MAX] = {
	cbox_hook_handler_0, cbox_hook_handler_1, cbox_hook_handler_2, cbox_hook_handler_3,
	cbox_hook_handler_4, cbox_hook_handler_5, cbox_hook_handler_6, cbox_hook_handler_7,
};

static zend_function *cbox_find_function(const char *class_name, const char *function_name)
{
	zend_class_entry *ce;
	zend_function *func;
	zend_string *lower;

	if (class_name == NULL) {
		lower = zend_string_init(function_name, strlen(function_name), 0);
		zend_str_tolower(ZSTR_VAL(lower), ZSTR_LEN(lower));
		func = (zend_function *) zend_hash_find_ptr(CG(function_table), lower);
		zend_string_release(lower);

		return func;
	}

	lower = zend_string_init(class_name, strlen(class_name), 0);
	zend_str_tolower(ZSTR_VAL(lower), ZSTR_LEN(lower));
	ce = (zend_class_entry *) zend_hash_find_ptr(CG(class_table), lower);
	zend_string_release(lower);

	if (ce == NULL) {
		return NULL;
	}

	lower = zend_string_init(function_name, strlen(function_name), 0);
	zend_str_tolower(ZSTR_VAL(lower), ZSTR_LEN(lower));
	func = (zend_function *) zend_hash_find_ptr(&ce->function_table, lower);
	zend_string_release(lower);

	return func;
}

static void cbox_hook_add(int group, const char *class_name, const char *function_name, cbox_op_type op)
{
	zend_function *target;
	cbox_hook *hook;

	if (cbox_hook_count >= CBOX_HOOK_MAX) {
		return;
	}

	target = cbox_find_function(class_name, function_name);

	/*
	 * Missing is normal, not an error: ext-redis may not be installed, and PDO
	 * spells its connection entry point differently across versions. It is
	 * still recorded, because "asked for but not present" and "asked for and
	 * working" look identical from the outside otherwise.
	 */
	if (target == NULL || target->type != ZEND_INTERNAL_FUNCTION) {
		cbox_hook_groups[group].missing++;
		return;
	}

	/* Somebody else got there first (another APM, Xdebug). Leave theirs alone. */
	if (target->internal_function.handler == NULL) {
		cbox_hook_groups[group].missing++;
		return;
	}

	hook = &cbox_hooks[cbox_hook_count];
	hook->class_name = class_name;
	hook->function_name = function_name;
	hook->op = op;
	hook->target = target;
	hook->original = target->internal_function.handler;
	hook->installed = true;

	target->internal_function.handler = cbox_hook_handlers[cbox_hook_count];
	cbox_hook_count++;

	cbox_hook_groups[group].installed++;

	if (cbox_hook_names_count < CBOX_HOOK_MAX) {
		if (class_name != NULL) {
			snprintf(cbox_hook_names[cbox_hook_names_count], sizeof(cbox_hook_names[0]),
				"%s::%s", class_name, function_name);
		} else {
			snprintf(cbox_hook_names[cbox_hook_names_count], sizeof(cbox_hook_names[0]),
				"%s", function_name);
		}

		cbox_hook_names_count++;
	}
}

static void cbox_hooks_build_summary(bool pdo, bool redis, bool curl, bool streams)
{
	cbox_hook_summary[0] = '\0';

	if (pdo) {
		strcat(cbox_hook_summary, "pdo");
	}

	if (redis) {
		strcat(cbox_hook_summary, cbox_hook_summary[0] != '\0' ? ",redis" : "redis");
	}

	if (curl) {
		strcat(cbox_hook_summary, cbox_hook_summary[0] != '\0' ? ",curl" : "curl");
	}

	if (streams) {
		strcat(cbox_hook_summary, cbox_hook_summary[0] != '\0' ? ",streams" : "streams");
	}

	if (cbox_hook_summary[0] == '\0') {
		strcat(cbox_hook_summary, "none");
	}
}

void cbox_hooks_install(bool pdo, bool redis, bool curl, bool streams)
{
	if (cbox_hooks_done) {
		return;
	}

	cbox_hooks_done = true;
	cbox_hooks_build_summary(pdo, redis, curl, streams);

	cbox_hook_groups[CBOX_GROUP_PDO].requested = pdo;
	cbox_hook_groups[CBOX_GROUP_REDIS].requested = redis;
	cbox_hook_groups[CBOX_GROUP_CURL].requested = curl;
	cbox_hook_groups[CBOX_GROUP_STREAMS].requested = streams;

	if (pdo) {
		cbox_hook_add(CBOX_GROUP_PDO, "PDO", "__construct", CBOX_OP_PDO_CONNECT);
		/* PHP 8.4 added PDO::connect() as a second way in. */
		cbox_hook_add(CBOX_GROUP_PDO, "PDO", "connect", CBOX_OP_PDO_CONNECT);
	}

	if (redis) {
		cbox_hook_add(CBOX_GROUP_REDIS, "Redis", "connect", CBOX_OP_REDIS_CONNECT);
		cbox_hook_add(CBOX_GROUP_REDIS, "Redis", "pconnect", CBOX_OP_REDIS_PCONNECT);
	}

	if (curl) {
		cbox_hook_add(CBOX_GROUP_CURL, NULL, "curl_exec", CBOX_OP_CURL_EXEC);
	}

	if (streams) {
		cbox_hook_add(CBOX_GROUP_STREAMS, NULL, "stream_socket_client", CBOX_OP_STREAM_CONNECT);
		cbox_hook_add(CBOX_GROUP_STREAMS, NULL, "fsockopen", CBOX_OP_STREAM_CONNECT);
	}
}

const cbox_hook_group *cbox_hooks_groups(uint32_t *count)
{
	*count = CBOX_GROUP_MAX;

	return cbox_hook_groups;
}

uint32_t cbox_hooks_installed_count(void)
{
	return cbox_hook_names_count;
}

const char *cbox_hooks_installed_name(uint32_t index)
{
	return index < cbox_hook_names_count ? cbox_hook_names[index] : NULL;
}

void cbox_hooks_uninstall(void)
{
	uint32_t index;

	for (index = 0; index < cbox_hook_count; index++) {
		cbox_hook *hook = &cbox_hooks[index];

		/*
		 * Only restore if the handler is still ours. If someone wrapped us
		 * afterwards, putting the original back would cut them out.
		 */
		if (hook->installed
			&& hook->target != NULL
			&& hook->target->internal_function.handler == cbox_hook_handlers[index]
		) {
			hook->target->internal_function.handler = hook->original;
		}

		hook->installed = false;
	}

	cbox_hook_count = 0;
	cbox_hooks_done = false;
}

const char *cbox_hooks_active(void)
{
	return cbox_hook_summary[0] == '\0' ? "none" : cbox_hook_summary;
}
