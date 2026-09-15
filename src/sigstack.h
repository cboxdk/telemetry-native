/*
 * cbox_telemetry — alternate signal stack.
 *
 * One per thread, shared by the sampling timer and the crash recorder.
 *
 * Both handlers need it, for different reasons. The crash handler needs it
 * because a stack-overflow SIGSEGV can only be caught off the overflowed
 * stack. The timer handler needs it because PHP Fibers switch the C stack
 * underneath us with make_fcontext/jump_fcontext, and a signal that lands in
 * that window would otherwise run on a stack pointer that belongs to neither
 * context.
 */
#ifndef CBOX_SIGSTACK_H
#define CBOX_SIGSTACK_H

#include <stdbool.h>

/* Idempotent. True when SA_ONSTACK is safe to set on a handler. */
bool cbox_sigstack_ensure(void);
void cbox_sigstack_release(void);

#endif /* CBOX_SIGSTACK_H */
