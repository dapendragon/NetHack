/* Yendor JSON-NL event emitter. Phase 1 wire format (one line per event):
 *   {"seq":N,"t":"<name>","ts_ms":M, ... key/value pairs ...}
 *
 * Two API layers:
 *   - High-level: unity_emit_event_begin() / kv_int|uint|str|bool /
 *     kv_obj_begin / kv_obj_end / event_end. Used by typed handlers in
 *     unity_callback.c (M5+).
 *   - Legacy: unity_emit_callback(name, fmt) emits just the format string
 *     for shim entries that don't yet have a typed handler. Filled in as
 *     M5/M6/M7 progress.
 *
 * Deliberately does NOT include hack.h. NetHack's headers macro-pollute
 * common identifiers (e.g. `uprops`) and break the Windows SDK headers if
 * <windows.h> is included afterwards. This file only needs the C runtime
 * and Win32 timing API, so it stays standalone. */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

static uint64_t seq_counter = 0;
static uint64_t start_tick_ms = 0;

/* UNITY_PORT: a side-band context request is scoped to the input event. */
unsigned long long
unity_emit_sequence(void)
{
    return (unsigned long long) seq_counter;
}

/* Comma-suppression stack. s_first_pending[d-1] == 1 means the next kv
 * written at depth d must NOT prefix a comma (it's the first key in that
 * scope). depth 0 = no event in flight. */
#define UNITY_EMIT_DEPTH 8
static int s_first_pending[UNITY_EMIT_DEPTH];
static int s_depth = 0;

void
unity_emit_init(void)
{
    /* Switch stdout to full-buffered with an 8KB buffer. Default on
     * Windows is line-buffered (or even unbuffered against a pipe),
     * which costs a kernel transition + context switch per event.
     * That's ~5ms per round-trip on Windows pipes — fine for a few
     * events, ruinous when the engine emits ~2000 print_glyph events
     * for a single map render. Full-buffered batches roughly 50-70
     * events per write; input-blocking handlers call unity_emit_flush()
     * before they block so the harness sees prompts promptly. */
    setvbuf(stdout, NULL, _IOFBF, 8192);

    seq_counter = 0;
    start_tick_ms = (uint64_t) GetTickCount64();
    s_depth = 0;
}

/* Force any buffered output out to the pipe. Call before blocking on
 * input — the harness needs to see the prompt before the engine waits.
 * Also worth calling before any explicit exit() / signal handler. */
void
unity_emit_flush(void)
{
    fflush(stdout);
}

static uint64_t
now_ms(void)
{
    return (uint64_t) GetTickCount64() - start_tick_ms;
}

/* Write a JSON-escaped copy of `s` to stdout.
 *
 * Strings come from upstream and may contain anything: pure ASCII
 * messages, UTF-8 strings (glyph_info.gm.u->utf8str), CP-437 box-
 * drawing chars in ttychar, etc. Output must be valid UTF-8 JSON or
 * downstream consumers (Python's text-mode reader) will crash.
 *
 * Strategy: pass through ASCII (0x20-0x7E) and well-formed UTF-8
 * multi-byte sequences verbatim; escape control chars (0x00-0x1F)
 * and any byte that doesn't start or continue a valid UTF-8 sequence
 * as \u00XX (treating it as Latin-1 codepoint). */
static void
emit_json_string(const char *s)
{
    const unsigned char *p;

    if (!s) {
        fputs("null", stdout);
        return;
    }
    fputc('"', stdout);
    p = (const unsigned char *) s;
    while (*p) {
        unsigned char c = *p;

        switch (c) {
        case '\\': fputs("\\\\", stdout); p++; continue;
        case '"':  fputs("\\\"", stdout); p++; continue;
        case '\n': fputs("\\n",  stdout); p++; continue;
        case '\r': fputs("\\r",  stdout); p++; continue;
        case '\t': fputs("\\t",  stdout); p++; continue;
        default: break;
        }

        if (c < 0x20) {
            fprintf(stdout, "\\u%04x", c);
            p++;
        } else if (c < 0x80) {
            fputc(c, stdout);
            p++;
        } else {
            /* High bit set: try to read a complete UTF-8 sequence. */
            int n;
            if      ((c & 0xE0) == 0xC0) n = 2;
            else if ((c & 0xF0) == 0xE0) n = 3;
            else if ((c & 0xF8) == 0xF0) n = 4;
            else                          n = 0;

            int valid = (n > 0);
            for (int i = 1; valid && i < n; i++) {
                if ((p[i] & 0xC0) != 0x80) valid = 0;
            }

            if (valid) {
                for (int i = 0; i < n; i++)
                    fputc(p[i], stdout);
                p += n;
            } else {
                /* Lone or malformed high byte — escape as Latin-1. */
                fprintf(stdout, "\\u%04x", c);
                p++;
            }
        }
    }
    fputc('"', stdout);
}

/* Write a separator before the next key in the current scope. */
static void
emit_sep(void)
{
    if (s_depth == 0)
        return;
    if (s_first_pending[s_depth - 1])
        s_first_pending[s_depth - 1] = 0;
    else
        fputc(',', stdout);
}

void
unity_emit_event_begin(const char *name)
{
    fprintf(stdout, "{\"seq\":%llu,\"t\":",
            (unsigned long long) ++seq_counter);
    emit_json_string(name ? name : "");
    fprintf(stdout, ",\"ts_ms\":%llu",
            (unsigned long long) now_ms());
    s_depth = 1;
    s_first_pending[0] = 0;
}

void
unity_emit_event_end(void)
{
    /* Auto-close any unbalanced object scopes so a handler bug can't
     * leave half a JSON line on the wire. */
    while (s_depth > 0) {
        fputc('}', stdout);
        s_depth--;
    }
    fputc('\n', stdout);
    /* Deliberate: no fflush here. stdout is full-buffered in
     * unity_emit_init; input-blocking handlers flush explicitly. */
}

void
unity_emit_kv_int(const char *key, long long v)
{
    emit_sep();
    emit_json_string(key);
    fprintf(stdout, ":%lld", v);
}

void
unity_emit_kv_uint(const char *key, unsigned long long v)
{
    emit_sep();
    emit_json_string(key);
    fprintf(stdout, ":%llu", v);
}

void
unity_emit_kv_str(const char *key, const char *s)
{
    emit_sep();
    emit_json_string(key);
    fputc(':', stdout);
    emit_json_string(s);
}

void
unity_emit_kv_bool(const char *key, int v)
{
    emit_sep();
    emit_json_string(key);
    fputs(v ? ":true" : ":false", stdout);
}

void
unity_emit_kv_obj_begin(const char *key)
{
    emit_sep();
    emit_json_string(key);
    fputs(":{", stdout);
    if (s_depth < UNITY_EMIT_DEPTH) {
        s_first_pending[s_depth] = 1;
        s_depth++;
    }
}

void
unity_emit_kv_obj_end(void)
{
    if (s_depth > 1) {
        fputc('}', stdout);
        s_depth--;
    }
}

/* UNITY_PORT: structured action arrays, sharing the emitter's scope stack. */
void
unity_emit_kv_array_begin(const char *key)
{
    emit_sep();
    emit_json_string(key);
    fputs(":[", stdout);
    s_first_pending[s_depth++] = 1;
}

void
unity_emit_array_object_begin(void)
{
    emit_sep();
    fputc('{', stdout);
    s_first_pending[s_depth++] = 1;
}

void
unity_emit_array_end(void)
{
    fputc(']', stdout);
    s_depth--;
}

/* Legacy fallback for shim entries without a typed handler. Emits just
 * the callback name and its format string so the harness sees the call
 * happened, even before we've wired up structured arguments. */
void
unity_emit_callback(const char *name, const char *fmt)
{
    const char *t = name ? name : "";
    if (strncmp(t, "shim_", 5) == 0)
        t += 5;
    unity_emit_event_begin(t);
    unity_emit_kv_str("fmt", fmt ? fmt : "");
    unity_emit_event_end();
}

/* Synthetic: session is set up and engine is about to start. */
void
unity_emit_session_ready(const char *sandbox_dir)
{
    unity_emit_event_begin("session_ready");
    unity_emit_kv_str("sandbox", sandbox_dir);
    unity_emit_event_end();
}
