/* Yendor input queue. A daemon thread reads JSON-NL commands from stdin
 * and pushes them into per-type blocking slots; the engine thread blocks
 * in shim_nhgetch / shim_yn_function / shim_getlin / shim_nh_poskey /
 * shim_get_ext_cmd until a matching item is available.
 *
 * Slot vs. queue: keys can arrive faster than the engine consumes them
 * (movement bursts), so they're FIFO-buffered. Text / poskey / ext_cmd
 * replies follow the prompt-response synchronous pattern, so each is a
 * single-slot rendezvous; if the harness pushes a second one before the
 * engine has consumed the first, the new one overwrites and we log to
 * stderr. Mismatched cmd types do NOT cross-contaminate (sending an
 * answer_text while the engine waits on a key just buffers the text).
 *
 * Deliberately does NOT include hack.h: NetHack's headers macro-pollute
 * common identifiers and break <windows.h> if included afterwards. We
 * use a local TEXT_LEN that mirrors NetHack's BUFSZ (256) with margin. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_QUEUE_CAP 256
#define TEXT_LEN      512  /* BUFSZ is 256; this gives margin */

/* Key FIFO. */
static int               key_buf[KEY_QUEUE_CAP];
static int               key_head = 0;
static int               key_tail = 0;
static int               key_count = 0;
static CONDITION_VARIABLE cv_key;

/* Text rendezvous (for getlin). */
static char              text_value[TEXT_LEN];
static int               text_present = 0;
static CONDITION_VARIABLE cv_text;

/* Position+key rendezvous (for nh_poskey). */
static struct {
    int x, y, mod, key;
}                        pos_value;
static int               pos_present = 0;
static CONDITION_VARIABLE cv_pos;

/* Extended command rendezvous (for get_ext_cmd). Index is into the
 * engine's extended command table; -1 means cancel. */
static int               ext_cmd_value;
static int               ext_cmd_present = 0;
static CONDITION_VARIABLE cv_ext_cmd;

static CRITICAL_SECTION  cs;
static HANDLE            reader_thread = NULL;
static volatile LONG     shutting_down = 0;

/* ---- minimal JSON parsing helpers ---- */

static const char *
skip_ws(const char *p)
{
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        p++;
    return p;
}

/* Parse a signed JSON integer at p. Returns pointer past the digits, or
 * NULL on malformed input. */
static const char *
parse_json_int(const char *p, int *out)
{
    int neg = 0, v = 0;

    p = skip_ws(p);
    if (*p == '-') { neg = 1; p++; }
    if (*p < '0' || *p > '9')
        return NULL;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    *out = neg ? -v : v;
    return p;
}

/* Parse a JSON string at p (must be pointing at the opening quote).
 * Decodes \\, \", \/, \b, \f, \n, \r, \t. Skips \uXXXX and writes '?' as
 * a placeholder (M5 doesn't need Unicode in name prompts). Other bytes
 * including high-bit UTF-8 pass through. Truncates at outlen-1 chars,
 * always null-terminates. Returns pointer past the closing quote, or
 * NULL on malformed input. */
static const char *
parse_json_str(const char *p, char *out, size_t outlen)
{
    size_t i = 0;

    p = skip_ws(p);
    if (*p != '"')
        return NULL;
    p++;

    while (*p && *p != '"') {
        char c;

        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case '\\': c = '\\'; p++; break;
            case '"':  c = '"';  p++; break;
            case '/':  c = '/';  p++; break;
            case 'b':  c = '\b'; p++; break;
            case 'f':  c = '\f'; p++; break;
            case 'n':  c = '\n'; p++; break;
            case 'r':  c = '\r'; p++; break;
            case 't':  c = '\t'; p++; break;
            case 'u': {
                int k;
                p++;
                for (k = 0; k < 4 && *p; k++)
                    p++;
                c = '?';
                break;
            }
            default:   c = *p;   p++; break;
            }
        } else {
            c = *p;
            p++;
        }
        if (i + 1 < outlen)
            out[i++] = c;
    }

    if (*p != '"')
        return NULL;
    if (outlen > 0)
        out[i] = '\0';
    return p + 1;
}

/* Skip an arbitrary JSON value at p (object, array, string, number, or
 * literal). Used when we encounter unknown keys we don't care about. */
static const char *
skip_json_value(const char *p)
{
    int depth = 0;

    p = skip_ws(p);
    while (*p) {
        if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            if (depth == 0)
                break;
            depth--;
        } else if (*p == ',' && depth == 0) {
            break;
        } else if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p += 2;
                else p++;
            }
            if (*p) p++;
            continue;
        }
        if (*p)
            p++;
    }
    return p;
}

/* ---- enqueue helpers (called under cs) ---- */

static void
push_key(int k)
{
    if (key_count < KEY_QUEUE_CAP) {
        key_buf[key_tail] = k;
        key_tail = (key_tail + 1) % KEY_QUEUE_CAP;
        key_count++;
        WakeConditionVariable(&cv_key);
    } else {
        fprintf(stderr, "yendor: key queue full, dropping\n");
    }
}

static void
push_text(const char *s)
{
    if (text_present)
        fprintf(stderr, "yendor: overwriting unconsumed answer_text\n");
    if (s) {
        strncpy(text_value, s, TEXT_LEN - 1);
        text_value[TEXT_LEN - 1] = '\0';
    } else {
        text_value[0] = '\0';
    }
    text_present = 1;
    WakeConditionVariable(&cv_text);
}

static void
push_pos(int x, int y, int mod, int key)
{
    if (pos_present)
        fprintf(stderr, "yendor: overwriting unconsumed answer_pos\n");
    pos_value.x = x;
    pos_value.y = y;
    pos_value.mod = mod;
    pos_value.key = key;
    pos_present = 1;
    WakeConditionVariable(&cv_pos);
}

static void
push_ext_cmd(int idx)
{
    if (ext_cmd_present)
        fprintf(stderr, "yendor: overwriting unconsumed answer_ext_cmd\n");
    ext_cmd_value = idx;
    ext_cmd_present = 1;
    WakeConditionVariable(&cv_ext_cmd);
}

/* ---- top-level command parser ---- */

/* Parse one JSON-NL command line. On success, enqueues the matching
 * item under cs and returns 0. Returns negative on a malformed line
 * (logged to stderr in stdin_reader). */
static int
parse_and_enqueue(const char *line)
{
    const char *p = skip_ws(line);
    char cmd[32] = {0};
    char text[TEXT_LEN] = {0};
    int  has_text = 0;
    int  key_val = -1;
    int  pos_x = -1, pos_y = -1, pos_mod = 0, pos_key = 0;
    int  ext_idx = -1;

    if (*p != '{')
        return -1;
    p++;

    while (*p) {
        char key[32] = {0};
        const char *next;

        p = skip_ws(p);
        if (*p == '}') break;

        next = parse_json_str(p, key, sizeof key);
        if (!next) return -1;
        p = next;

        p = skip_ws(p);
        if (*p != ':') return -1;
        p++;

        if (strcmp(key, "cmd") == 0) {
            next = parse_json_str(p, cmd, sizeof cmd);
            if (!next) return -1;
            p = next;
        } else if (strcmp(key, "value") == 0) {
            /* Used by `key` (single-char string OR int) and
             * `answer_text` (full string). Decide by leading char. */
            p = skip_ws(p);
            if (*p == '"') {
                next = parse_json_str(p, text, sizeof text);
                if (!next) return -1;
                p = next;
                has_text = 1;
                /* For `key` cmd we treat first byte as the keycode. */
                if (text[0])
                    key_val = (unsigned char) text[0];
            } else {
                next = parse_json_int(p, &key_val);
                if (!next) return -1;
                p = next;
            }
        } else if (strcmp(key, "x") == 0) {
            next = parse_json_int(p, &pos_x);
            if (!next) return -1;
            p = next;
        } else if (strcmp(key, "y") == 0) {
            next = parse_json_int(p, &pos_y);
            if (!next) return -1;
            p = next;
        } else if (strcmp(key, "mod") == 0) {
            next = parse_json_int(p, &pos_mod);
            if (!next) return -1;
            p = next;
        } else if (strcmp(key, "key") == 0) {
            next = parse_json_int(p, &pos_key);
            if (!next) return -1;
            p = next;
        } else if (strcmp(key, "index") == 0) {
            next = parse_json_int(p, &ext_idx);
            if (!next) return -1;
            p = next;
        } else {
            p = skip_json_value(p);
        }

        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') break;
        return -1;
    }

    /* Dispatch on cmd. */
    EnterCriticalSection(&cs);
    if (strcmp(cmd, "key") == 0 && key_val >= 0) {
        push_key(key_val);
    } else if (strcmp(cmd, "answer_text") == 0) {
        push_text(has_text ? text : "");
    } else if (strcmp(cmd, "answer_pos") == 0) {
        push_pos(pos_x, pos_y, pos_mod, pos_key);
    } else if (strcmp(cmd, "answer_ext_cmd") == 0) {
        push_ext_cmd(ext_idx);
    } else {
        LeaveCriticalSection(&cs);
        fprintf(stderr, "yendor: ignoring unsupported command \"%s\"\n", cmd);
        return -2;
    }
    LeaveCriticalSection(&cs);
    return 0;
}

static DWORD WINAPI
stdin_reader(LPVOID arg)
{
    char line[4096];
    (void) arg;

    while (!shutting_down && fgets(line, sizeof line, stdin)) {
        if (parse_and_enqueue(line) < 0)
            fprintf(stderr, "yendor: bad input line: %s", line);
    }
    /* On EOF, wake any blocked consumers so they don't deadlock. */
    EnterCriticalSection(&cs);
    InterlockedExchange(&shutting_down, 1);
    WakeAllConditionVariable(&cv_key);
    WakeAllConditionVariable(&cv_text);
    WakeAllConditionVariable(&cv_pos);
    WakeAllConditionVariable(&cv_ext_cmd);
    LeaveCriticalSection(&cs);
    return 0;
}

/* ---- public API ---- */

void
unity_input_queue_init(void)
{
    InitializeCriticalSection(&cs);
    InitializeConditionVariable(&cv_key);
    InitializeConditionVariable(&cv_text);
    InitializeConditionVariable(&cv_pos);
    InitializeConditionVariable(&cv_ext_cmd);
    reader_thread = CreateThread(NULL, 0, stdin_reader, NULL, 0, NULL);
}

void
unity_input_queue_shutdown(void)
{
    InterlockedExchange(&shutting_down, 1);
    if (reader_thread) {
        /* Stdin is blocking; closing it would kill the read but is
         * platform-specific. M4/M5 accept that the thread may linger
         * if the harness keeps stdin open. */
        CloseHandle(reader_thread);
        reader_thread = NULL;
    }
    DeleteCriticalSection(&cs);
}

int
unity_input_queue_pop_key(void)
{
    int v;
    EnterCriticalSection(&cs);
    while (key_count == 0 && !shutting_down)
        SleepConditionVariableCS(&cv_key, &cs, INFINITE);
    if (key_count == 0) {
        LeaveCriticalSection(&cs);
        return -1;
    }
    v = key_buf[key_head];
    key_head = (key_head + 1) % KEY_QUEUE_CAP;
    key_count--;
    LeaveCriticalSection(&cs);
    return v;
}

void
unity_input_queue_pop_text(char *out, size_t outlen)
{
    EnterCriticalSection(&cs);
    while (!text_present && !shutting_down)
        SleepConditionVariableCS(&cv_text, &cs, INFINITE);
    if (!text_present) {
        LeaveCriticalSection(&cs);
        if (outlen > 0) {
            /* NetHack convention: bufp[0]='\033' = cancel. */
            out[0] = '\033';
            if (outlen > 1) out[1] = '\0';
        }
        return;
    }
    if (outlen > 0) {
        strncpy(out, text_value, outlen - 1);
        out[outlen - 1] = '\0';
    }
    text_present = 0;
    LeaveCriticalSection(&cs);
}

int
unity_input_queue_pop_poskey(int *x, int *y, int *mod)
{
    int k;
    EnterCriticalSection(&cs);
    while (!pos_present && !shutting_down)
        SleepConditionVariableCS(&cv_pos, &cs, INFINITE);
    if (!pos_present) {
        LeaveCriticalSection(&cs);
        if (x) *x = -1;
        if (y) *y = -1;
        if (mod) *mod = 0;
        return '\033';
    }
    if (x) *x = pos_value.x;
    if (y) *y = pos_value.y;
    if (mod) *mod = pos_value.mod;
    k = pos_value.key;
    pos_present = 0;
    LeaveCriticalSection(&cs);
    return k;
}

int
unity_input_queue_pop_ext_cmd(void)
{
    int v;
    EnterCriticalSection(&cs);
    while (!ext_cmd_present && !shutting_down)
        SleepConditionVariableCS(&cv_ext_cmd, &cs, INFINITE);
    if (!ext_cmd_present) {
        LeaveCriticalSection(&cs);
        return -1;
    }
    v = ext_cmd_value;
    ext_cmd_present = 0;
    LeaveCriticalSection(&cs);
    return v;
}
