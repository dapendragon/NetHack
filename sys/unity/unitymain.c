/* Yendor headless entry point. M3: parse --sandbox <dir>, set up the
 * JSON-NL emitter, point all NetHack file-prefix slots at the sandbox,
 * register the shim callback, and walk just enough of the windmain.c
 * startup sequence to fire init_nhwindows. M4 adds the input queue. */

#include "hack.h"

extern void shim_graphics_set_callback(
    void (*cb)(const char *name, void *ret_ptr, const char *fmt, ...));
extern void unity_shim_callback(
    const char *name, void *ret_ptr, const char *fmt, ...);

extern void        unity_emit_init(void);
extern void        unity_emit_session_ready(const char *sandbox_dir);
extern const char *unity_paths_extract_sandbox(int *argc, char *argv[]);
extern void        unity_paths_set_sandbox(const char *dir);

int
main(int argc, char *argv[])
{
    const char *sandbox_dir;

    unity_emit_init();
    sandbox_dir = unity_paths_extract_sandbox(&argc, argv);

    early_init(argc, argv);
    gh.hname = "NetHack";
    set_default_prefix_locations(argv[0]);
    if (sandbox_dir)
        unity_paths_set_sandbox(sandbox_dir);

    shim_graphics_set_callback(unity_shim_callback);
    choose_windows("shim");

    unity_emit_session_ready(sandbox_dir);

    init_nhwindows(&argc, argv);
    /* M3 still ends here; the engine doesn't yet have a real game loop
     * because there's no input queue (M4). */
    exit_nhwindows("M3: emitter + sandbox paths verified.");
    return 0;
}
