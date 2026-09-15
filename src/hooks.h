/*
 * cbox_telemetry — native operation hooks.
 *
 * Exact begin/end timing for the handful of calls where Laravel's events
 * cannot see the truth: connection establishment and cURL transfers. Sampling
 * cannot answer "why did opening this connection take 180 ms" — only a pair of
 * timestamps around the call can.
 *
 * These are targeted internal-handler swaps, not Zend Observer registrations.
 * That is a measured decision, not a preference: registering *any* observer
 * puts the whole engine onto its observed-call path, and on call-heavy PHP
 * that measured ~20% slower on every workload we tried, whether or not any
 * observed function was ever called. Swapping the handler of a
 * dozen named functions costs exactly nothing for the other few thousand.
 *
 * See docs/decisions/0001-operation-hook-mechanism.md.
 */
#ifndef CBOX_HOOKS_H
#define CBOX_HOOKS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Install the hooks. Must run after every extension's MINIT — the classes we
 * patch belong to other extensions and may not exist yet during ours — so this
 * is called once from the first request, not from MINIT.
 */
void cbox_hooks_install(bool pdo, bool redis, bool curl, bool streams);

/* Put the original handlers back. MSHUTDOWN only. */
void cbox_hooks_uninstall(void);

/* A short, comma-separated list of what actually got hooked. */
const char *cbox_hooks_active(void);

/*
 * What was asked for versus what exists.
 *
 * "redis is enabled" says nothing about whether ext-redis is installed, and an
 * operator staring at a dashboard with no redis.connect timings needs to know
 * which of the two it is. Configuration is an intention; this is the outcome.
 */
typedef struct _cbox_hook_group {
	const char *label;     /* "pdo", "redis", "curl", "streams" */
	bool        requested; /* the INI asked for it */
	uint32_t    installed; /* targets found and wrapped */
	uint32_t    missing;   /* targets that do not exist in this build */
} cbox_hook_group;

const cbox_hook_group *cbox_hooks_groups(uint32_t *count);

/* Names of the functions actually wrapped, for diagnostics. */
uint32_t    cbox_hooks_installed_count(void);
const char *cbox_hooks_installed_name(uint32_t index);

#endif /* CBOX_HOOKS_H */
