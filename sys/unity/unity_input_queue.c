/* Yendor input queue (M4). A daemon thread reads JSON-NL commands from
 * stdin and pushes them into a blocking, thread-safe queue; the engine
 * thread blocks in shim_nhgetch / shim_yn_function until a matching item
 * is available. M4 only handles `key` commands — single-int values used
 * by both nhgetch (returns int) and yn_function (returns char). M5+ adds
 * menu_pick / answer_text / answer_dir / answer_ext_cmd / shutdown.
 *
 * Deliberately does NOT include hack.h: NetHack's headers macro-pollute
 * common identifiers and break <windows.h> if included afterwards. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUEUE_CAP 256

static int               key_buf[QUEUE_CAP];
static int               key_head = 0;
static int               key_tail = 0;
static int               key_count = 0;
static CRITICAL_SECTION  cs;
static CONDITION_VARIABLE cv_not_empty;
static HANDLE            reader_thread = NULL;
static volatile LONG     shutting_down = 0;

/* Skip ASCII whitespace. */
static const char *
skip_ws(const char *p)
{
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        p++;
    return p;
}

/* If `p` points at the JSON string literal "key", consume it and return
 * the pointer past the closing quote. Otherwise return NULL. Does NOT
 * handle escape sequences — fine for our controlled input format where
 * keys are short tag strings. */
static const char *
match_string_literal(const char *p, const char *want)
{
    p = skip_ws(p);
    if (*p != '"') return NULL;
    p++;
    size_t n = strlen(want);
    if (strncmp(p, want, n) != 0) return NULL;
    if (p[n] != '"') return NULL;
    return p + n + 1;
}

/* Parse one JSON-NL command line. Returns 0 on success (item enqueued),
 * negative on a malformed/unsupported line (logged to stderr). */
static int
parse_and_enqueue(const char *line)
{
    /* Expected shape: {"cmd":"key","value":"X"} where X is a single char.
     * Parser is intentionally loose — order of keys can vary, whitespace
     * is allowed. */
    const char *p = skip_ws(line);
    if (*p != '{') return -1;
    p++;

    char cmd[32] = {0};
    int  key_val = -1;

    while (*p) {
        p = skip_ws(p);
        if (*p == '}') break;

        /* Read a key. */
        if (*p != '"') return -1;
        p++;
        const char *key_start = p;
        while (*p && *p != '"') p++;
        if (*p != '"') return -1;
        size_t klen = (size_t) (p - key_start);
        p++; /* past closing " */
        p = skip_ws(p);
        if (*p != ':') return -1;
        p++;
        p = skip_ws(p);

        /* Read a value (string or number). */
        if (klen == 3 && strncmp(key_start, "cmd", 3) == 0) {
            if (*p != '"') return -1;
            p++;
            const char *vs = p;
            while (*p && *p != '"') p++;
            if (*p != '"') return -1;
            size_t n = (size_t) (p - vs);
            if (n >= sizeof cmd) n = sizeof cmd - 1;
            memcpy(cmd, vs, n);
            cmd[n] = '\0';
            p++;
        } else if (klen == 5 && strncmp(key_start, "value", 5) == 0) {
            if (*p == '"') {
                p++;
                if (*p == '\\') p++; /* tolerate one escape */
                key_val = (unsigned char) *p;
                if (*p) p++;
                while (*p && *p != '"') p++;
                if (*p == '"') p++;
            } else {
                /* Parse an unsigned integer. */
                key_val = 0;
                while (*p >= '0' && *p <= '9') {
                    key_val = key_val * 10 + (*p - '0');
                    p++;
                }
            }
        } else {
            /* Unknown key — skip its value. Best-effort: walk until comma
             * or closing brace at depth zero. */
            int depth = 0;
            while (*p) {
                if (*p == '{' || *p == '[') depth++;
                else if (*p == '}' || *p == ']') {
                    if (depth == 0) break;
                    depth--;
                } else if (*p == ',' && depth == 0) break;
                else if (*p == '"') {
                    p++;
                    while (*p && *p != '"') p++;
                }
                if (*p) p++;
            }
        }

        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') break;
        return -1;
    }

    if (strcmp(cmd, "key") == 0 && key_val >= 0) {
        EnterCriticalSection(&cs);
        if (key_count < QUEUE_CAP) {
            key_buf[key_tail] = key_val;
            key_tail = (key_tail + 1) % QUEUE_CAP;
            key_count++;
            WakeConditionVariable(&cv_not_empty);
        } else {
            fprintf(stderr, "yendor: input queue full, dropping key\n");
        }
        LeaveCriticalSection(&cs);
        return 0;
    }

    fprintf(stderr, "yendor: ignoring unsupported command \"%s\"\n", cmd);
    return -2;
}

static DWORD WINAPI
stdin_reader(LPVOID arg)
{
    (void) arg;
    char line[4096];
    while (!shutting_down && fgets(line, sizeof line, stdin)) {
        if (parse_and_enqueue(line) < 0)
            fprintf(stderr, "yendor: bad input line: %s", line);
    }
    /* On EOF, wake any blocked consumers so they don't deadlock. */
    EnterCriticalSection(&cs);
    InterlockedExchange(&shutting_down, 1);
    WakeAllConditionVariable(&cv_not_empty);
    LeaveCriticalSection(&cs);
    return 0;
}

void
unity_input_queue_init(void)
{
    InitializeCriticalSection(&cs);
    InitializeConditionVariable(&cv_not_empty);
    reader_thread = CreateThread(NULL, 0, stdin_reader, NULL, 0, NULL);
}

void
unity_input_queue_shutdown(void)
{
    InterlockedExchange(&shutting_down, 1);
    if (reader_thread) {
        /* Stdin is blocking; closing it would kill the read but is
         * platform-specific. For M4 we accept the thread may linger
         * if the harness keeps stdin open. */
        CloseHandle(reader_thread);
        reader_thread = NULL;
    }
    DeleteCriticalSection(&cs);
}

/* Block until a key is available; return -1 if stdin closed. */
int
unity_input_queue_pop_key(void)
{
    int v;
    EnterCriticalSection(&cs);
    while (key_count == 0 && !shutting_down)
        SleepConditionVariableCS(&cv_not_empty, &cs, INFINITE);
    if (key_count == 0) {
        LeaveCriticalSection(&cs);
        return -1;
    }
    v = key_buf[key_head];
    key_head = (key_head + 1) % QUEUE_CAP;
    key_count--;
    LeaveCriticalSection(&cs);
    return v;
}
