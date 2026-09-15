dnl config.m4 for extension cbox_telemetry
dnl
dnl Linux is the supported production target (per-thread CPU timers via
dnl timer_create + SIGEV_THREAD_ID). Anything without them falls back to the
dnl sampler thread in src/timer_thread.c, where profiling is off by default —
dnl see KNOWN-ISSUES.md.

PHP_ARG_ENABLE([cbox-telemetry],
  [whether to enable cbox_telemetry support],
  [AS_HELP_STRING([--enable-cbox-telemetry],
    [Enable cbox_telemetry support])],
  [no])

if test "$PHP_CBOX_TELEMETRY" != "no"; then

  AC_DEFINE([HAVE_CBOX_TELEMETRY], [1], [Define to 1 if cbox_telemetry is enabled])

  dnl ---------------------------------------------------------------- timer
  CBOX_TIMER_SOURCE=""

  dnl
  dnl Per-thread CPU timers need both SIGEV_THREAD_ID and a way to write the
  dnl target thread id into struct sigevent. There is no portable spelling for
  dnl the latter: glibc leaves the POSIX-looking name to the kernel headers and
  dnl expects _sigev_un._tid, musl uses __sev_fields.__tid. Probe rather than
  dnl guess, and fall back to setitimer if none of them compiles.
  dnl
  CBOX_SIGEV_TID_FIELD=""

  AC_CHECK_DECL([SIGEV_THREAD_ID], [cbox_have_sigev_thread_id=yes],
    [cbox_have_sigev_thread_id=no], [[
      #define _GNU_SOURCE 1
      #include <signal.h>
      #include <time.h>
    ]])

  dnl One explicit probe per spelling. A shell loop around AC_COMPILE_IFELSE
  dnl looks tidier but does not survive every autoconf version — the quoted
  dnl list gets eaten and configure dies with a syntax error.
  AC_DEFUN([CBOX_TRY_SIGEV_FIELD], [
    if test -z "$CBOX_SIGEV_TID_FIELD"; then
      AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
        #define _GNU_SOURCE 1
        #include <signal.h>
        #include <time.h>
      ]], [[
        struct sigevent event;
        event.$1 = 0;
        (void) event;
      ]])], [CBOX_SIGEV_TID_FIELD="$1"])
    fi
  ])

  if test "$cbox_have_sigev_thread_id" = "yes"; then
    AC_MSG_CHECKING([how to set the notify thread id on struct sigevent])

    dnl The POSIX-looking name (when the kernel headers provide it),
    dnl then glibc's spelling, then musl's.
    CBOX_TRY_SIGEV_FIELD([sigev_notify_thread_id])
    CBOX_TRY_SIGEV_FIELD([_sigev_un._tid])
    CBOX_TRY_SIGEV_FIELD([__sev_fields.__tid])

    if test -n "$CBOX_SIGEV_TID_FIELD"; then
      AC_MSG_RESULT([$CBOX_SIGEV_TID_FIELD])
    else
      AC_MSG_RESULT([no usable member])
    fi
  fi

  if test -n "$CBOX_SIGEV_TID_FIELD"; then
    dnl Timer overruns tell us how many ticks the kernel had to drop while the
    dnl VM was somewhere it could not be interrupted. Without them, long
    dnl internal calls collapse into a single sample.
    AC_CHECK_MEMBER([siginfo_t.si_overrun],
      [AC_DEFINE([CBOX_HAVE_SI_OVERRUN], [1],
        [Define to 1 if siginfo_t carries si_overrun])],
      [], [[
        #define _GNU_SOURCE 1
        #include <signal.h>
      ]])

    AC_CHECK_LIB([rt], [timer_create],
      [PHP_ADD_LIBRARY([rt], [], [CBOX_TELEMETRY_SHARED_LIBADD])])
    AC_DEFINE([CBOX_TIMER_POSIX], [1],
      [Define to 1 when per-thread POSIX CPU timers are available])
    AC_DEFINE_UNQUOTED([CBOX_SIGEV_TID_FIELD], [$CBOX_SIGEV_TID_FIELD],
      [The struct sigevent member that carries the notify thread id])
    CBOX_TIMER_SOURCE="src/timer_posix.c"
  else
    AC_DEFINE([CBOX_TIMER_THREAD], [1],
      [Define to 1 when falling back to the wall-clock timer thread])
    CBOX_TIMER_SOURCE="src/timer_thread.c"
  fi

  AC_CHECK_DECL([gettid],
    [AC_DEFINE([HAVE_GETTID], [1], [Define to 1 if gettid() is declared])],
    [], [[
      #define _GNU_SOURCE 1
      #include <unistd.h>
    ]])

  dnl --------------------------------------------------------------- pthread
  AC_SEARCH_LIBS([pthread_kill], [pthread],
    [PHP_EVAL_LIBLINE([$LIBS], [CBOX_TELEMETRY_SHARED_LIBADD])])

  dnl ---------------------------------------------------------- crash recorder
  AC_CHECK_FUNCS([sigaltstack flock clock_gettime])

  dnl Keep our symbols to ourselves; only the module entry needs to be visible.
  AX_CHECK_COMPILE_FLAG([-fvisibility=hidden],
    [CFLAGS="$CFLAGS -fvisibility=hidden"])

  PHP_SUBST([CBOX_TELEMETRY_SHARED_LIBADD])

  dnl The list must not end with a newline before the closing bracket:
  dnl PHP_NEW_EXTENSION expands it straight into `for ac_src in <list>; do`,
  dnl and a trailing newline there terminates the word list and leaves a bare
  dnl `; do` behind.
  PHP_NEW_EXTENSION([cbox_telemetry], [cbox_telemetry.c \
    src/arena.c \
    src/sigstack.c \
    src/frames.c \
    src/stacktree.c \
    src/profiler.c \
    src/unit.c \
    src/ops.c \
    src/hooks.c \
    src/breadcrumbs.c \
    src/crash.c \
    $CBOX_TIMER_SOURCE], [$ext_shared], , [-DZEND_ENABLE_STATIC_TSRMLS_CACHE=1])

  PHP_ADD_BUILD_DIR([$ext_builddir/src])
fi
