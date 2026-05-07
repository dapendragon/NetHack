/* Signal + atexit flush so the harness sees panic messages even when
 * the engine dies hard. stdout is full-buffered (set by unity_emit_init)
 * so any raw_print output emitted right before the crash sits in the
 * buffer and gets dropped if the OS terminates us before the C runtime
 * can flush. We install handlers for SIGSEGV / SIGABRT / SIGFPE / SIGILL
 * (all common abnormal-exit signals on Windows MSVCRT) that flush stdout
 * and re-raise the original signal with the default disposition so the
 * OS still produces a crash dump / nonzero exit code.
 *
 * Plus an _invalid_parameter_handler: MSVCRT calls abort() on certain
 * CRT function violations (bad format string, buffer overrun, etc.).
 * Default Watson handler swallows context; we capture file/line/etc.
 * and emit a fatal_error event before letting the process die.
 *
 * Deliberately does NOT include hack.h: we only need the C runtime
 * and forward declarations. */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <stdint.h>
#include <windows.h>

extern void unity_emit_flush(void);

static void
fatal_handler(int sig)
{
    fprintf(stderr, "[unity_signals] fatal_handler sig=%d\n", sig);
    fflush(stderr);
    /* Restore the default disposition so a re-raise actually crashes. */
    signal(sig, SIG_DFL);
    unity_emit_flush();
    fflush(stderr);
    raise(sig);
}

/* Vectored exception handler — first-chance, fires before SEH unwind.
 * We use it to flush stdout so that any buffered events emitted right
 * before a hardware-level fault still reach the harness. Skip the
 * harmless OutputDebugString first-chance exceptions (0x40010006 /
 * 0x4001000a) the engine and Lua emit. Always return CONTINUE_SEARCH
 * so the normal handler chain still runs. */
static LONG WINAPI
vectored_handler(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    if (code == 0x40010006 || code == 0x4001000a)
        return EXCEPTION_CONTINUE_SEARCH;

    fprintf(stderr, "[unity_signals] VEH code=0x%08lx addr=%p\n",
            (unsigned long) code, ep->ExceptionRecord->ExceptionAddress);
    fflush(stderr);
    unity_emit_flush();
    return EXCEPTION_CONTINUE_SEARCH;
}

static void
on_exit_flush(void)
{
    unity_emit_flush();
    fflush(stderr);
}

/* Print a wide string to stderr safely (it might be NULL). */
static void
fputws_or(const wchar_t *s, const wchar_t *fallback)
{
    fwprintf(stderr, L"%ls", s ? s : fallback);
}

static void
invalid_parameter_handler(
    const wchar_t *expression,
    const wchar_t *function,
    const wchar_t *file,
    unsigned int line,
    uintptr_t reserved)
{
    (void) reserved;
    fputs("\n[unity_signals] _invalid_parameter:\n", stderr);
    fputs("  expression: ", stderr); fputws_or(expression, L"<null>");
    fputs("\n  function:   ", stderr); fputws_or(function, L"<null>");
    fputs("\n  file:       ", stderr); fputws_or(file, L"<null>");
    fprintf(stderr, "\n  line:       %u\n", line);
    fflush(stderr);
    unity_emit_flush();
    /* Don't return — that lets MSVCRT continue past the violation,
     * usually corrupting state further. Abort cleanly. */
    abort();
}

void
unity_signals_init(void)
{
    signal(SIGSEGV, fatal_handler);
    signal(SIGABRT, fatal_handler);
    signal(SIGFPE,  fatal_handler);
    signal(SIGILL,  fatal_handler);
    signal(SIGTERM, fatal_handler);
    atexit(on_exit_flush);

    /* Replace MSVCRT's default Watson handler so CRT-detected violations
     * (bad printf format, buffer overruns, NULL arg to a *_s function...)
     * surface details to stderr instead of vanishing into a silent abort. */
    _set_invalid_parameter_handler(invalid_parameter_handler);
    /* Suppress the "abort: this program has requested..." popup dialog. */
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    /* Vectored exception handler — sees SEH exceptions before any other
     * handler chain. First-chance handler (1 = called before SEH frame
     * walk). Used for tracing only; we let the OS unwind normally. */
    AddVectoredExceptionHandler(1, vectored_handler);
}
