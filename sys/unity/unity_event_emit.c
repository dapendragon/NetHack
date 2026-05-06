/* Yendor JSON-NL event emitter. Phase 1 wire format (one line per event):
 *   {"seq": N, "t": "<name>", "ts_ms": M, "fmt": "<format>"}
 * For synthetic events (session_ready, fatal_error, …) extra keys are
 * appended before the closing brace.
 *
 * Args themselves are NOT yet serialized — that lights up at M5 once each
 * shim entry has a real handler that interprets its format string. */

/* Deliberately does NOT include hack.h. NetHack's headers macro-pollute
 * common identifiers (e.g. `uprops`) and break the Windows SDK headers if
 * <windows.h> is included afterwards. This file only needs the C runtime
 * and Win32 timing API, so it stays standalone. */

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

static uint64_t seq_counter = 0;
static uint64_t start_tick_ms = 0;

void
unity_emit_init(void)
{
    seq_counter = 0;
    start_tick_ms = (uint64_t) GetTickCount64();
}

static uint64_t
now_ms(void)
{
    return (uint64_t) GetTickCount64() - start_tick_ms;
}

/* Write a JSON-escaped copy of `s` to stdout. Escapes \ and " and control
 * chars; everything else passes through. UTF-8 bytes are valid JSON. */
static void
emit_json_string(const char *s)
{
    if (!s) {
        fputs("null", stdout);
        return;
    }
    fputc('"', stdout);
    for (; *s; ++s) {
        unsigned char c = (unsigned char) *s;
        switch (c) {
        case '\\': fputs("\\\\", stdout); break;
        case '"':  fputs("\\\"", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default:
            if (c < 0x20)
                fprintf(stdout, "\\u%04x", c);
            else
                fputc(c, stdout);
        }
    }
    fputc('"', stdout);
}

/* Emit one shim callback as a JSON-NL line. */
void
unity_emit_callback(const char *name, const char *fmt)
{
    const char *t = name ? name : "";
    /* Drop the "shim_" prefix so wire-format names match the plan. */
    if (strncmp(t, "shim_", 5) == 0)
        t += 5;

    fprintf(stdout, "{\"seq\":%llu,\"t\":",
            (unsigned long long) ++seq_counter);
    emit_json_string(t);
    fprintf(stdout, ",\"ts_ms\":%llu,\"fmt\":",
            (unsigned long long) now_ms());
    emit_json_string(fmt ? fmt : "");
    fputs("}\n", stdout);
    fflush(stdout);
}

/* Synthetic event: session_ready. Emitted once unitymain has finished
 * setting up paths / callbacks and is about to enter the engine. */
void
unity_emit_session_ready(const char *sandbox_dir)
{
    fprintf(stdout, "{\"seq\":%llu,\"t\":\"session_ready\",\"ts_ms\":%llu,"
                    "\"sandbox\":",
            (unsigned long long) ++seq_counter,
            (unsigned long long) now_ms());
    emit_json_string(sandbox_dir);
    fputs("}\n", stdout);
    fflush(stdout);
}
