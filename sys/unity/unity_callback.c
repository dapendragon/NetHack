/* Shim windowport callback. Forwards each call to the JSON-NL emitter.
 * M5 will replace the trivial fmt-only emit with per-event-type handlers
 * that serialize the variadic args; for M3 we just emit the metadata. */

extern void unity_emit_callback(const char *name, const char *fmt);

void
unity_shim_callback(const char *name, void *ret_ptr, const char *fmt, ...)
{
    (void) ret_ptr;
    unity_emit_callback(name, fmt);
}
