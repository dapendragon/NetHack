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

/* tty_procs is referenced by src/windows.c's winchoices[] table. We never
 * select it (we choose "shim"), but the iteration code dereferences .name,
 * so a non-NULL name string is required. */
struct window_procs tty_procs = { "tty-disabled", 0 };
void win_tty_init(int when) { (void) when; }

/* windsound_procs is referenced by src/sounds.c's sound-provider table.
 * Same story: never selected because we don't link the windsound backend. */
struct sound_procs windsound_procs = { "windsound-disabled", 0 };
