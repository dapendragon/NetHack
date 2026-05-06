/* Yendor M2 stub: prints each shim windowport call name to stdout.
 * Replaced in M3 by the JSON-NL emitter. */

#include <stdio.h>
#include <stdarg.h>

void
unity_shim_callback(const char *name, void *ret_ptr, const char *fmt, ...)
{
    (void) ret_ptr;
    (void) fmt;
    printf("CB: %s\n", name);
    fflush(stdout);
}
