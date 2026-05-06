/* Shim windowport callback. Forwards every call to the JSON-NL emitter
 * and, for the input-returning calls, blocks on the input queue and
 * writes the result into *ret_ptr.
 *
 * M4 covers shim_nhgetch (int) and shim_yn_function (char). The other
 * input-returning calls (nh_poskey, getlin, get_ext_cmd, select_menu)
 * still return zero-init defaults; M5/M6 fill those in as the matching
 * UI flows light up. */

#include <string.h>

extern void unity_emit_callback(const char *name, const char *fmt);
extern int  unity_input_queue_pop_key(void);

void
unity_shim_callback(const char *name, void *ret_ptr, const char *fmt, ...)
{
    unity_emit_callback(name, fmt);

    if (!name || !ret_ptr)
        return;

    if (strcmp(name, "shim_nhgetch") == 0) {
        int k = unity_input_queue_pop_key();
        *(int *) ret_ptr = (k < 0) ? 0 : k;
    } else if (strcmp(name, "shim_yn_function") == 0) {
        int k = unity_input_queue_pop_key();
        *(char *) ret_ptr = (char) ((k < 0) ? '\033' : k);
    }
}
