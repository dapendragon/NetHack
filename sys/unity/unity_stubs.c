/* Stubs for symbols normally provided by sys/windows/{windmain,consoletty}.c,
 * win/tty/*.c, and sound/windsound/windsound.c — all of which the Yendor
 * build deliberately excludes (we have our own main and no console/sound
 * glue). The shared engine objects and sys/windows/windsys.c still
 * reference these names, so we provide minimal definitions to satisfy
 * the linker. None of these stubs run on the Phase 1 happy path. */

#include "hack.h"
#include "winprocs.h"
#include "sndprocs.h"

#include <stdio.h>
#include <stdarg.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>

/* Globals from windmain.c / consoletty.c that windsys.o references. */
boolean getreturn_enabled = TRUE;
int GUILaunched = 0;

/* From windmain.c — empty no-ops are fine for M2; replaced as the
 * Phase 1 milestones light up code paths that need real behavior. */
void backsp(void) { }
void free_winmain_stuff(void) { }
void port_help(void) { }
boolean authorize_wizard_mode(void) { return FALSE; }
boolean authorize_explore_mode(void) { return TRUE; }
void chdirx(const char *dir, boolean wr)
{
    (void) dir; (void) wr;
}

/* From consoletty.c. set_altkeyhandling and set_keyhandling_via_option
 * concern Win32 console input modes — irrelevant headless. */
void set_altkeyhandling(const char *inName) { (void) inName; }
int  set_keyhandling_via_option(void) { return 0; }

/* From wintty.c / windconf.c / windsys.c-error-path. */
void map_subkeyvalue(char *s) { (void) s; }
void term_end_screen(void) { }
void xputs(const char *s) { if (s) fputs(s, stderr); }
void msmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* getlock() lives in sys/windows/windmain.c — which we don't link.
 * The full version handles the lock_file(HLOCK) mutex and the
 * SELF_RECOVER prompt; we don't need either (single-process headless,
 * no save recovery prompt). What we DO need is the level-0 placeholder
 * file at the gotlock: label (windmain.c:1118-1145) — without it, the
 * first level access fails ENOENT and the player dies on turn 1. */
int
getlock(void)
{
    const char *fq_lock;
    int fd;

    set_levelfile_name(gl.lock, 0);
    fq_lock = fqname(gl.lock, LEVELPREFIX, 1);
    fd = creat(fq_lock, FCMASK);
    if (fd == -1) {
        char oops[256];
        Sprintf(oops, "cannot creat %s: %s", fq_lock, strerror(errno));
        raw_print(oops);
        return 0;
    }
    (void) write(fd, (char *) &svh.hackpid, sizeof svh.hackpid);
    (void) nhclose(fd);
    return 1;
}

/* tty_procs is referenced by src/windows.c's winchoices[] table. We never
 * select it (we choose "shim"), but the iteration code dereferences .name,
 * so a non-NULL name string is required. */
struct window_procs tty_procs = { "tty-disabled", 0 };
void win_tty_init(int when) { (void) when; }

/* windsound_procs is referenced by src/sounds.c's sound-provider table.
 * Same story: never selected because we don't link the windsound backend. */
struct sound_procs windsound_procs = { "windsound-disabled", 0 };
