/* Yendor headless entry point. M5: bring up the real engine flow.
 *
 * Modeled on sys/libnh/libnhmain.c's main, with Unix-specific bits
 * (passwd, signal handling, environment lookups) stripped out and our
 * own --sandbox / --name CLI extraction in their place.
 *
 * Initialization order matters and follows libnhmain.c precisely:
 *   1. emit/queue/path init                (Yendor-specific)
 *   2. early_init                           (engine global setup)
 *   3. initoptions                          (incl. sf_init for save-format procs)
 *   4. choose_windows("shim") + register the shim callback
 *   5. init_nhwindows                       (windowport hello)
 *   6. set_playmode + plnamesuffix          (parse role/race from name)
 *   7. dlb_init / vision_init / init_sound_disp_gamewindows
 *   8. getlock + optional restore_saved_game
 *   9. player_selection + newgame on new
 *  10. moveloop(resuming) — the engine never returns from here
 */

#include "hack.h"
#include "dlb.h"

#include <stdlib.h>
#include <string.h>
#include <process.h>

extern void shim_graphics_set_callback(
    void (*cb)(const char *name, void *ret_ptr, const char *fmt, ...));
extern void unity_shim_callback(
    const char *name, void *ret_ptr, const char *fmt, ...);

/* From sys/windows/windsys.c. Not in any public header — windmain.c
 * declares it the same way. Returns void; without this prototype MSVC
 * implicitly declares it `int` (C4013) and silently passes garbage. */
extern void set_default_prefix_locations(const char *programPath);

extern void        unity_emit_init(void);
extern void        unity_emit_session_ready(const char *sandbox_dir);
extern const char *unity_paths_extract_sandbox(int *argc, char *argv[]);
extern void        unity_paths_set_sandbox(const char *dir);
extern void        unity_input_queue_init(void);
extern void        unity_input_queue_shutdown(void);
extern void        unity_signals_init(void);
extern void        unity_seed_set(unsigned long seed);
extern void        unity_emit_manifest(void);

/* Pull `--<flag> <value>` out of argv, removing both tokens. Must be
 * called before argv is handed to early_init(). Returns the value (still
 * owned by argv's storage) or NULL if the flag wasn't present. */
static const char *
extract_kv_flag(const char *flag, int *argc, char *argv[])
{
    int i, j;

    for (i = 1; i + 1 < *argc; i++) {
        if (strcmp(argv[i], flag) == 0) {
            const char *val = argv[i + 1];
            for (j = i; j + 2 <= *argc; j++)
                argv[j] = argv[j + 2];
            *argc -= 2;
            return val;
        }
    }
    return NULL;
}

int
main(int argc, char *argv[])
{
    const char *sandbox_dir;
    const char *name_arg;
    const char *seed_arg;
    boolean resuming = FALSE;
    NHFILE *nhfp;

    unity_emit_init();
    unity_signals_init();
    sandbox_dir = unity_paths_extract_sandbox(&argc, argv);
    name_arg = extract_kv_flag("--name", &argc, argv);
    /* --seed N: deterministic RNG for save/restore and movement tests.
     * Set before initoptions() runs init_random() (see options.c:7161). */
    seed_arg = extract_kv_flag("--seed", &argc, argv);
    if (seed_arg)
        unity_seed_set(strtoul(seed_arg, NULL, 0));
    unity_input_queue_init();

    early_init(argc, argv);
    gh.hname = "NetHack";
    /* Every other port's main sets this — libnhmain.c:85 via getpid(),
     * windmain.c:401 via GetCurrentProcessId. Without it, getlock()
     * writes 0 into the lock placeholder file and downstream stale-lock
     * detection misbehaves. */
    svh.hackpid = (long) _getpid();
    set_default_prefix_locations(argv[0]);
    if (sandbox_dir)
        unity_paths_set_sandbox(sandbox_dir);

    shim_graphics_set_callback(unity_shim_callback);
    choose_windows("shim");

    /* initoptions() runs sf_init() (populates sfiprocs/sfoprocs save-
     * format function-pointer tables — without them the engine's first
     * sfi_int32() call crashes with a NULL dispatch), allopt_array_init,
     * config-file parse, etc. Every other port's main calls this; we
     * missed it for a while and chased the resulting crash deep into
     * newgame(). */
    initoptions();

    /* Plant the player name before plnamesuffix() runs so its role/race
     * suffix parsing has something to work with. The harness should
     * always pass --name; without it, plnamesuffix() will call askname(),
     * which under our shim is a no-op and the engine proceeds with
     * whatever default it falls back to. */
    if (name_arg) {
        strncpy(svp.plname, name_arg, sizeof svp.plname - 1);
        svp.plname[sizeof svp.plname - 1] = '\0';
    }

    unity_emit_session_ready(sandbox_dir);

    init_nhwindows(&argc, argv);
    /* Real ports set this inside their init_nhwindows; the shim doesn't.
     * Without it, panic()'s "Oops..." raw_print is skipped and any
     * engine-side panic exits silently. */
    iflags.window_inited = TRUE;

    set_playmode();
    gp.plnamelen = 0;
    plnamesuffix();

    (void) dlb_init();
    vision_init();
    init_sound_disp_gamewindows();

    /* getlock() serializes concurrent runs and creates the empty
     * `<name>.0` level-lock placeholder file the engine reads later
     * when laying out dungeon levels. */
    if (*svp.plname) {
        getlock();
        nhfp = restore_saved_game();
        if (nhfp) {
            pline("Restoring save file...");
            mark_synch();
            if (dorecover(nhfp))
                resuming = TRUE;
        }
    }

    if (!resuming) {
        player_selection();
        newgame();
    }

    /* One-shot content manifest (index->name for monsters/objects). Emitted
     * here, after newgame()/restore, because OBJ_NAME() dereferences the
     * oc_name_idx that init_objects() assigns inside newgame(). The Unity
     * side consumes this to build modder-facing name aliases without
     * transcribing NGPL'd name tables — see unity_emit_manifest(). */
    unity_emit_manifest();

    /* moveloop never returns from a normal play session — death, save+
     * quit, and panic all exit the process directly. */
    moveloop(resuming);

    unity_input_queue_shutdown();
    return 0;
}
