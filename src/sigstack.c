#include "sigstack.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

/* Generous: the crash handler copies a 2 KB record snapshot onto it. */
#define CBOX_SIGSTACK_BYTES 65536

static char *cbox_sigstack_memory = NULL;
static bool  cbox_sigstack_ready = false;

bool cbox_sigstack_ensure(void)
{
	stack_t desired;
	stack_t existing;

	if (cbox_sigstack_ready) {
		return true;
	}

	/*
	 * Somebody else (another extension, the host application) may already
	 * have installed one. Theirs is as good as ours — take it rather than
	 * replace it, since replacing would break their handlers.
	 */
	if (sigaltstack(NULL, &existing) == 0
		&& existing.ss_sp != NULL
		&& (existing.ss_flags & SS_DISABLE) == 0
	) {
		cbox_sigstack_ready = true;
		return true;
	}

	cbox_sigstack_memory = (char *) malloc(CBOX_SIGSTACK_BYTES);

	if (cbox_sigstack_memory == NULL) {
		return false;
	}

	desired.ss_sp = cbox_sigstack_memory;
	desired.ss_size = CBOX_SIGSTACK_BYTES;
	desired.ss_flags = 0;

	if (sigaltstack(&desired, NULL) != 0) {
		free(cbox_sigstack_memory);
		cbox_sigstack_memory = NULL;
		return false;
	}

	cbox_sigstack_ready = true;

	return true;
}

void cbox_sigstack_release(void)
{
	stack_t disable;

	if (cbox_sigstack_memory != NULL) {
		memset(&disable, 0, sizeof(disable));
		disable.ss_flags = SS_DISABLE;
		sigaltstack(&disable, NULL);

		free(cbox_sigstack_memory);
		cbox_sigstack_memory = NULL;
	}

	cbox_sigstack_ready = false;
}
