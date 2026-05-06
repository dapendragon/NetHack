/* Yendor M2 entry point. Bare-minimum headless main: register the shim
 * callback, pick the shim windowport, fire init_nhwindows, exit. M3+ adds
 * --sandbox, --seed, role/race/etc. parsing and the real game loop. */

#include "hack.h"

extern void shim_graphics_set_callback(
    void (*cb)(const char *name, void *ret_ptr, const char *fmt, ...));
extern void unity_shim_callback(
    const char *name, void *ret_ptr, const char *fmt, ...);

int
main(int argc, char *argv[])
{
    early_init(argc, argv);
    gh.hname = "NetHack";
    set_default_prefix_locations(argv[0]);

    shim_graphics_set_callback(unity_shim_callback);
    choose_windows("shim");

    init_nhwindows(&argc, argv);
    /* M2 success criterion: callback fired. Tear down through the shim
     * before the engine looks for input. */
    exit_nhwindows("M2: shim wiring verified.");
    return 0;
}
