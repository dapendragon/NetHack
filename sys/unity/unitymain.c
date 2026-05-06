/* Yendor headless entry point. M4: starts the stdin reader thread before
 * any input-blocking shim call could fire, then drives the windowport
 * through one nhgetch round-trip to verify the queue end-to-end. M5+
 * replaces the synthetic nhgetch with the real game flow. */

#include "hack.h"

extern void shim_graphics_set_callback(
    void (*cb)(const char *name, void *ret_ptr, const char *fmt, ...));
extern void unity_shim_callback(
    const char *name, void *ret_ptr, const char *fmt, ...);

extern void        unity_emit_init(void);
extern void        unity_emit_session_ready(const char *sandbox_dir);
extern const char *unity_paths_extract_sandbox(int *argc, char *argv[]);
extern void        unity_paths_set_sandbox(const char *dir);
extern void        unity_input_queue_init(void);
extern void        unity_input_queue_shutdown(void);

int
main(int argc, char *argv[])
{
    const char *sandbox_dir;
    int unused_key;

    unity_emit_init();
    sandbox_dir = unity_paths_extract_sandbox(&argc, argv);
    unity_input_queue_init();

    early_init(argc, argv);
    gh.hname = "NetHack";
    set_default_prefix_locations(argv[0]);
    if (sandbox_dir)
        unity_paths_set_sandbox(sandbox_dir);

    shim_graphics_set_callback(unity_shim_callback);
    choose_windows("shim");

    unity_emit_session_ready(sandbox_dir);

    init_nhwindows(&argc, argv);

    /* M4 verification: drive one full input round-trip. The harness must
     * pipe a `{"cmd":"key","value":"X"}` line on stdin; otherwise this
     * blocks until stdin closes. */
    unused_key = (*windowprocs.win_nhgetch)();
    (void) unused_key;

    exit_nhwindows("M4: input queue verified.");
    unity_input_queue_shutdown();
    return 0;
}
