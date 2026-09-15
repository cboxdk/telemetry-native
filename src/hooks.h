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
 * observed function was ever called. Swapping the handler of six named
 * functions costs exactly nothing for the other few thousand.
 *
 * See docs/decisions/0001-operation-hook-mechanism.md.
 */
#ifndef CBOX_HOOKS_H
#define CBOX_HOOKS_H

#include <stdbool.h>

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

#endif /* CBOX_HOOKS_H */
