/* Shim windowport callback. Forwards every shim entry to the JSON-NL
 * emitter and, for the input-returning calls, blocks on the input queue
 * and writes the result into *ret_ptr.
 *
 * M5 in progress: typed handlers cover map (print_glyph), messages
 * (putstr / raw_print / raw_print_bold), status (status_init /
 * _enablefield / _update), window lifecycle (init / create / clear /
 * display / destroy / exit / suspend / resume), cursor + viewport
 * (curs / cliparound), and the simplest input flows (nhgetch /
 * yn_function). All other shim entries fall through to the legacy
 * fmt-string-only emit; they gain typed handlers callback by callback.
 *
 * The remaining input-returning calls (nh_poskey, getlin, get_ext_cmd,
 * select_menu, message_menu, doprev_message) still return zero-init
 * defaults; M5 Tier 2 / M6 fill those in. */

#include "hack.h"
#include "func_tab.h"  /* extcmdlist[] + IFBURIED/AUTOCOMPLETE/... flags;
                        * not pulled in by hack.h (only cmd.c includes it) */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Forward-declared so we don't need to expose it via a header. Mirrors
 * the struct in unity_input_queue.c — kept in sync by hand. */
struct unity_menu_pick {
    long long id;
    long      count;
};

extern int  unity_input_queue_pop_key(void);
extern void unity_input_queue_pop_text(char *out, size_t outlen);
extern int  unity_input_queue_pop_poskey(int *x, int *y, int *mod);
extern int  unity_input_queue_pop_ext_cmd(void);
extern int  unity_input_queue_pop_menu_pick(struct unity_menu_pick *out,
                                            int outcap,
                                            int expected_menu_id);
extern void unity_emit_callback(const char *name, const char *fmt);
extern void unity_emit_event_begin(const char *name);
extern void unity_emit_event_end(void);
extern void unity_emit_flush(void);
extern void unity_emit_kv_int(const char *key, long long v);
extern void unity_emit_kv_uint(const char *key, unsigned long long v);
extern void unity_emit_kv_str(const char *key, const char *s);
extern void unity_emit_kv_bool(const char *key, int v);
extern void unity_emit_kv_obj_begin(const char *key);
extern void unity_emit_kv_obj_end(void);

/* ---- glyph decoder helper ---- */

/* Decode a single glyph_info into key/value pairs inside the currently-
 * open object scope. Caller is responsible for opening/closing the scope
 * via unity_emit_kv_obj_begin / unity_emit_kv_obj_end.
 *
 * Discriminator-by-field-presence: at most one *_idx (or *_frame /
 * *_pos / warn_level) is emitted; the harness/renderer dispatches on
 * which key is present. flags (the MG_* bitset) carries variant
 * information (hero/pet/corpse/statue/invis/nothing/unexplored/...).
 *
 * Order matters: bodies and statues live in their own glyph offset
 * ranges and must be checked BEFORE glyph_is_object, which (per
 * display.h:877-879) intentionally subsumes them. glyph_is_monster
 * does not subsume them, but we still keep body/statue first for
 * defensive ordering against future upstream changes. */
static void
emit_glyph_fields(const glyph_info *gi)
{
    int g;

    if (!gi)
        return;
    g = gi->glyph;

    if (glyph_is_invisible(g)) {
        /* No idx; flags carries MG_INVIS. */
    } else if (glyph_is_body(g)) {
        unity_emit_kv_int("mon_idx", glyph_to_body_corpsenm(g));
    } else if (glyph_is_statue(g)) {
        unity_emit_kv_int("mon_idx", glyph_to_statue_corpsenm(g));
    } else if (glyph_is_monster(g)) {
        unity_emit_kv_int("mon_idx", glyph_to_mon(g));
    } else if (glyph_is_object(g)) {
        unity_emit_kv_int("obj_idx", glyph_to_obj(g));
    } else if (glyph_is_cmap(g)) {
        unity_emit_kv_int("feat_idx", glyph_to_cmap(g));
    } else if (glyph_is_trap(g)) {
        unity_emit_kv_int("trap_idx", glyph_to_trap(g));
    } else if (glyph_is_warning(g)) {
        unity_emit_kv_int("warn_level", glyph_to_warning(g));
    } else if (glyph_is_swallow(g)) {
        /* Swallow encodes (swallower_mon_idx, position) in 8-glyph
         * groups: see display.h GLYPH_SWALLOW_OFF range. */
        unity_emit_kv_int("mon_idx", (g - GLYPH_SWALLOW_OFF) >> 3);
        unity_emit_kv_int("swallow_pos", glyph_to_swallow(g));
    } else if (glyph_is_explosion(g)) {
        unity_emit_kv_int("explode_frame", glyph_to_explosion(g));
    } else if (glyph_is_cmap_zap(g)) {
        /* (zap_type << 2) | direction; harness can split if needed. */
        unity_emit_kv_int("zap_frame", g - GLYPH_ZAP_OFF);
    }
    /* glyph_is_unexplored / glyph_is_nothing: no idx, MG_UNEXPL /
     * MG_NOTHING in flags. */

    unity_emit_kv_uint("flags", (unsigned long long) gi->gm.glyphflags);

    /* u8: prefer ENHANCED_SYMBOLS unicode rep, fall back to ttychar. */
#ifdef ENHANCED_SYMBOLS
    if (gi->gm.u && gi->gm.u->utf8str) {
        unity_emit_kv_str("u8", (const char *) gi->gm.u->utf8str);
    } else {
        char buf[2];
        buf[0] = (char) gi->ttychar;
        buf[1] = '\0';
        unity_emit_kv_str("u8", buf);
    }
#else
    {
        char buf[2];
        buf[0] = (char) gi->ttychar;
        buf[1] = '\0';
        unity_emit_kv_str("u8", buf);
    }
#endif
}

/* ---- typed handlers ---- */

/* All handlers share the same signature so they can live in one
 * dispatch table. Pure-emit handlers ignore ret_ptr; ret-writing /
 * input-blocking handlers use it. */

static void
handle_print_glyph(va_list ap, void *ret_ptr)
{
    int w  = va_arg(ap, int);  /* winid */
    int x  = va_arg(ap, int);  /* coordxy promoted to int */
    int y  = va_arg(ap, int);
    const glyph_info *gi  = va_arg(ap, const glyph_info *);
    const glyph_info *bgi = va_arg(ap, const glyph_info *);
    (void) ret_ptr;

    unity_emit_event_begin("print_glyph");
    unity_emit_kv_int("w", w);
    unity_emit_kv_int("x", x);
    unity_emit_kv_int("y", y);
    unity_emit_kv_obj_begin("g");
    emit_glyph_fields(gi);
    unity_emit_kv_obj_end();
    unity_emit_kv_obj_begin("bg");
    emit_glyph_fields(bgi);
    unity_emit_kv_obj_end();
    unity_emit_event_end();
}

static void
handle_putstr(va_list ap, void *ret_ptr)
{
    int w = va_arg(ap, int);
    int attr = va_arg(ap, int);
    const char *s = va_arg(ap, const char *);
    (void) ret_ptr;

    unity_emit_event_begin("putstr");
    unity_emit_kv_int("w", w);
    unity_emit_kv_int("attr", attr);
    unity_emit_kv_str("str", s);
    unity_emit_event_end();
}

/* Used by raw_print / raw_print_bold / exit_nhwindows / suspend_nhwindows,
 * all of which take a single `const char *str`. NetHack passes NULL for
 * the optional cases (exit_nhwindows in end.c:140/409/1580/1583 and
 * cmd.c:5204; suspend likewise on hangup). Omit the key entirely on
 * NULL so the schema can keep `str` typed as a plain string and so a
 * real raw_print(NULL) — a caller bug — surfaces as a missing-required
 * key. */
static void
emit_one_string_event(const char *event_name, va_list ap)
{
    const char *s = va_arg(ap, const char *);
    unity_emit_event_begin(event_name);
    if (s)
        unity_emit_kv_str("str", s);
    unity_emit_event_end();
}

static void
handle_raw_print(va_list ap, void *ret_ptr)
{
    (void) ret_ptr;
    emit_one_string_event("raw_print", ap);
}

static void
handle_raw_print_bold(va_list ap, void *ret_ptr)
{
    (void) ret_ptr;
    emit_one_string_event("raw_print_bold", ap);
}

static void
handle_status_init(va_list ap, void *ret_ptr)
{
    (void) ap;
    (void) ret_ptr;
    unity_emit_event_begin("status_init");
    unity_emit_event_end();
}

static void
handle_status_enablefield(va_list ap, void *ret_ptr)
{
    int fldidx = va_arg(ap, int);
    const char *nm = va_arg(ap, const char *);
    const char *field_fmt = va_arg(ap, const char *);
    int enable = va_arg(ap, int);  /* boolean promoted */
    (void) ret_ptr;

    unity_emit_event_begin("status_enablefield");
    unity_emit_kv_int("fldidx", fldidx);
    unity_emit_kv_str("nm", nm);
    unity_emit_kv_str("field_fmt", field_fmt);
    unity_emit_kv_bool("enable", enable);
    unity_emit_event_end();
}

/* shim_status_update fmt is "vipiiip":
 *   int fldidx, void *ptr, int chg, int percent, int color, ulong *colormasks
 * The ptr arg is a variant: NULL for BL_RESET/BL_FLUSH, long* (bitmask)
 * for BL_CONDITION, char* (pre-formatted text) for all other fields.
 * See wintty.c:4463-4490. The engine packs color: low byte is CLR_*,
 * high byte is the attr mask.
 *
 * M5 omits colormasks (an array of unsigned long, BL_ATTCLR_MAX entries,
 * used only for fancy condition hilites). Wire it up when needed. */
static void
handle_status_update(va_list ap, void *ret_ptr)
{
    int fldidx = va_arg(ap, int);
    void *ptr = va_arg(ap, void *);
    int chg = va_arg(ap, int);
    int percent = va_arg(ap, int);
    int color_packed = va_arg(ap, int);
    /* unsigned long *colormasks = va_arg(ap, unsigned long *); -- unused */
    (void) ret_ptr;

    unity_emit_event_begin("status_update");
    unity_emit_kv_int("fldidx", fldidx);

    if (fldidx == BL_RESET || fldidx == BL_FLUSH) {
        unity_emit_event_end();
        return;
    }

    if (fldidx == BL_CONDITION) {
        long *condptr = (long *) ptr;
        unity_emit_kv_uint("cond_bits",
                           (unsigned long long) (condptr ? (unsigned long) *condptr : 0UL));
    } else {
        unity_emit_kv_str("str", (const char *) ptr);
    }

    unity_emit_kv_int("chg", chg);
    unity_emit_kv_int("percent", percent);
    unity_emit_kv_int("color", color_packed & 0xFF);
    unity_emit_kv_int("attr", (color_packed >> 8) & 0xFF);
    unity_emit_event_end();
}

/* ---- window lifecycle ---- */

static void
handle_init_nhwindows(va_list ap, void *ret_ptr)
{
    /* Args are int *argcp, char **argv. Engine passes them so a real
     * windowport can consume its own command-line options. We don't
     * have any; emit and leave argv unchanged. */
    (void) ap;
    (void) ret_ptr;
    unity_emit_event_begin("init_nhwindows");
    unity_emit_event_end();
}

/* Sequential winid allocator. WIN_ERR (-1) is the only reserved value
 * upstream uses (wintype.h:146); ids from 1 are safe. Engine stores the
 * return value verbatim; we just need uniqueness. */
static int
alloc_next_winid(void)
{
    static int next = 1;
    return next++;
}

static void
handle_create_nhwindow(va_list ap, void *ret_ptr)
{
    int type = va_arg(ap, int);  /* NHW_MESSAGE / _STATUS / _MAP / ... */
    int wid = alloc_next_winid();

    unity_emit_event_begin("create_nhwindow");
    unity_emit_kv_int("type", type);
    unity_emit_kv_int("winid", wid);
    unity_emit_event_end();

    if (ret_ptr)
        *(winid *) ret_ptr = (winid) wid;
}

static void
handle_clear_nhwindow(va_list ap, void *ret_ptr)
{
    int w = va_arg(ap, int);
    (void) ret_ptr;
    unity_emit_event_begin("clear_nhwindow");
    unity_emit_kv_int("w", w);
    unity_emit_event_end();
}

/* Emit the set of currently-visible map cells as a "vision" event.
 *
 * Why a dedicated event rather than a per-cell flag on print_glyph:
 * flush_screen() only re-emits print_glyph for cells whose *glyph*
 * changed (gptr->gnew). A cell leaving the hero's sight keeps the same
 * remembered terrain glyph, so it is NOT re-emitted — a vis bit ridden
 * on print_glyph would go stale. The vision array (gv.viz_array) is the
 * authority and is current here: flush_screen() runs vision_recalc()
 * before its final display_nhwindow(WIN_MAP). So we snapshot cansee()
 * across the whole map on every map flush.
 *
 * Encoding: space-separated "x,y" decimal pairs in a single string
 * field — keeps the existing kv_str emit path (no array support in the
 * emitter) and stays trivially parseable on the C# side. The visible
 * set is small in practice (a lit room/corridor, well under COLNO*ROWNO).
 */
static void
emit_vision(void)
{
    /* Worst case every cell visible: "79,20 " is 6 chars, COLNO*ROWNO
     * cells → ~10 KB. Round up generously and bound-check each append. */
    static char buf[COLNO * ROWNO * 8];
    size_t len = 0;
    coordxy x, y;

    for (y = 0; y < ROWNO; y++) {
        for (x = 0; x < COLNO; x++) {
            if (!cansee(x, y))
                continue;
            /* "x,y " — leave room for the largest pair plus space + NUL. */
            if (len + 12 >= sizeof buf)
                goto done; /* defensive: never overrun */
            len += (size_t) snprintf(buf + len, sizeof buf - len,
                                     "%s%d,%d", len ? " " : "", x, y);
        }
    }
done:
    buf[len] = '\0';
    unity_emit_event_begin("vision");
    unity_emit_kv_str("cells", buf);
    unity_emit_event_end();
}

static void
handle_display_nhwindow(va_list ap, void *ret_ptr)
{
    int w = va_arg(ap, int);
    int blocking = va_arg(ap, int);  /* boolean promoted */
    (void) ret_ptr;
    unity_emit_event_begin("display_nhwindow");
    unity_emit_kv_int("w", w);
    unity_emit_kv_bool("blocking", blocking);
    unity_emit_event_end();

    /* The map window's flush is our once-per-display vision tick. */
    if (w == WIN_MAP)
        emit_vision();
}

static void
handle_destroy_nhwindow(va_list ap, void *ret_ptr)
{
    int w = va_arg(ap, int);
    (void) ret_ptr;
    unity_emit_event_begin("destroy_nhwindow");
    unity_emit_kv_int("w", w);
    unity_emit_event_end();
}

static void
handle_exit_nhwindows(va_list ap, void *ret_ptr)
{
    (void) ret_ptr;
    emit_one_string_event("exit_nhwindows", ap);
}

static void
handle_suspend_nhwindows(va_list ap, void *ret_ptr)
{
    (void) ret_ptr;
    emit_one_string_event("suspend_nhwindows", ap);
}

static void
handle_resume_nhwindows(va_list ap, void *ret_ptr)
{
    (void) ap;
    (void) ret_ptr;
    unity_emit_event_begin("resume_nhwindows");
    unity_emit_event_end();
}

static void
handle_curs(va_list ap, void *ret_ptr)
{
    int w = va_arg(ap, int);
    int x = va_arg(ap, int);
    int y = va_arg(ap, int);
    (void) ret_ptr;
    unity_emit_event_begin("curs");
    unity_emit_kv_int("w", w);
    unity_emit_kv_int("x", x);
    unity_emit_kv_int("y", y);
    unity_emit_event_end();
}

static void
handle_cliparound(va_list ap, void *ret_ptr)
{
    int x = va_arg(ap, int);
    int y = va_arg(ap, int);
    (void) ret_ptr;
    unity_emit_event_begin("cliparound");
    unity_emit_kv_int("x", x);
    unity_emit_kv_int("y", y);
    unity_emit_event_end();
}

/* ---- player setup + inventory heads-up ---- */

/* shim_askname is the windowport entry the engine calls to fetch the
 * player's name. NetHack 5.0 has no genl_askname helper, so each port
 * is responsible for writing svp.plname itself. We emit and return; the
 * engine proceeds with whatever name was supplied via the --name CLI
 * flag in unitymain.c. Harnesses should always pass --name. */
static void
handle_askname(va_list ap, void *ret_ptr)
{
    char namebuf[PL_NSIZ];

    (void) ap;
    (void) ret_ptr;

    unity_emit_event_begin("askname");
    unity_emit_event_end();
    unity_emit_flush();

    /* shim_askname is "v" (no args, no return). NetHack expects the
     * windowport to fill svp.plname[] itself (src/role.c:1692). Block on a
     * text answer (answer_text, same as getlin) and copy it in. An empty
     * answer leaves plname untouched, so the engine treats it as "no name"
     * and proceeds to pick/ask as usual. */
    namebuf[0] = '\0';
    unity_input_queue_pop_text(namebuf, sizeof namebuf);
    if (namebuf[0]) {
        (void) strncpy(svp.plname, namebuf, sizeof svp.plname - 1);
        svp.plname[sizeof svp.plname - 1] = '\0';
    }
}

/* shim_player_selection: similar story. The engine drives role/race/
 * gender/alignment selection through subsequent yn_function / getlin
 * calls when CLI flags don't fully constrain the choice. */
static void
handle_player_selection(va_list ap, void *ret_ptr)
{
    (void) ap;
    (void) ret_ptr;
    unity_emit_event_begin("player_selection");
    unity_emit_event_end();
}

/* shim_update_inventory: heads-up that inventory changed. Actual
 * contents arrive via the menu callbacks when the player presses 'i'. */
static void
handle_update_inventory(va_list ap, void *ret_ptr)
{
    int a1 = va_arg(ap, int);
    (void) ret_ptr;
    unity_emit_event_begin("update_inventory");
    unity_emit_kv_int("a1", a1);
    unity_emit_event_end();
}

/* ---- input-returning handlers ---- */

static void
handle_nhgetch(va_list ap, void *ret_ptr)
{
    (void) ap;
    unity_emit_event_begin("nhgetch");
    unity_emit_event_end();
    unity_emit_flush();
    if (ret_ptr) {
        int k = unity_input_queue_pop_key();
        *(int *) ret_ptr = (k < 0) ? 0 : k;
    }
}

static void
handle_yn_function(va_list ap, void *ret_ptr)
{
    const char *query = va_arg(ap, const char *);
    const char *resp = va_arg(ap, const char *);
    int def = va_arg(ap, int);  /* char promoted */
    char def_buf[2];

    def_buf[0] = (char) def;
    def_buf[1] = '\0';

    unity_emit_event_begin("yn_function");
    unity_emit_kv_str("query", query);
    unity_emit_kv_str("resp", resp);
    unity_emit_kv_str("def", def_buf);
    unity_emit_event_end();
    unity_emit_flush();

    if (ret_ptr) {
        int k = unity_input_queue_pop_key();
        /* Harness is responsible for sending a key in `resp`; we don't
         * second-guess it. ESC on stdin close keeps the engine moving. */
        *(char *) ret_ptr = (char) ((k < 0) ? '\033' : k);
    }
}

/* ---- menu emitters + interactive picks ---- */

/* M6: each shim_start_menu allocates a session-unique menu_id that gets
 * surfaced in the start_menu / end_menu / select_menu events so the
 * harness can correlate its menu_pick reply back to the right prompt. */
static int s_next_menu_id = 1;
static int s_current_menu_id = 0;

/* shim_start_menu fmt is "vii": winid, unsigned long mbehavior.
 * mbehavior is a flag bitfield (MENU_BEHAVE_*); we surface it raw and
 * let the harness/renderer interpret. */
static void
handle_start_menu(va_list ap, void *ret_ptr)
{
    int w = va_arg(ap, int);
    unsigned long mbeh = va_arg(ap, unsigned long);
    (void) ret_ptr;

    s_current_menu_id = s_next_menu_id++;

    unity_emit_event_begin("start_menu");
    unity_emit_kv_int("w", w);
    unity_emit_kv_uint("mbehavior", (unsigned long long) mbeh);
    unity_emit_kv_int("menu_id", s_current_menu_id);
    unity_emit_event_end();
}

/* shim_add_menu fmt is "vipi00iisi":
 *   winid, glyph_info*, ANY_P*, char ch, char gch, int attr, int clr,
 *   str, unsigned int itemflags
 * The 'i' for ANY_P* in fmt is a WASM-decoder quirk; on the native
 * variadic path the arg is a pointer. We dereference and surface the
 * union's a_int64 as `id` — covers pointer (8 bytes on x64), int, long,
 * etc. The harness echoes the value back via menu_pick at M6. */
static void
handle_add_menu(va_list ap, void *ret_ptr)
{
    int w = va_arg(ap, int);
    const glyph_info *gi = va_arg(ap, const glyph_info *);
    const ANY_P *id_ptr = va_arg(ap, const ANY_P *);
    int ch = va_arg(ap, int);
    int gch = va_arg(ap, int);
    int attr = va_arg(ap, int);
    int clr = va_arg(ap, int);
    const char *s = va_arg(ap, const char *);
    unsigned int itemflags = va_arg(ap, unsigned int);
    char ch_buf[2], gch_buf[2];
    long long id_val = id_ptr ? (long long) id_ptr->a_int64 : 0;
    (void) ret_ptr;

    ch_buf[0] = (char) ch;  ch_buf[1] = '\0';
    gch_buf[0] = (char) gch; gch_buf[1] = '\0';

    unity_emit_event_begin("add_menu");
    unity_emit_kv_int("w", w);
    unity_emit_kv_int("menu_id", s_current_menu_id);
    unity_emit_kv_int("id", id_val);
    unity_emit_kv_str("ch", ch_buf);
    unity_emit_kv_str("gch", gch_buf);
    unity_emit_kv_int("attr", attr);
    unity_emit_kv_int("clr", clr);
    unity_emit_kv_str("str", s);
    unity_emit_kv_uint("itemflags", (unsigned long long) itemflags);
    if (gi) {
        unity_emit_kv_obj_begin("g");
        emit_glyph_fields(gi);
        unity_emit_kv_obj_end();
    }
    unity_emit_event_end();
}

/* shim_end_menu fmt is "vis": winid, prompt str.
 * NetHack passes prompt=NULL for menus without a title (the common
 * case for inventory/pickup); omit the key entirely rather than
 * emitting JSON null so the schema can keep `prompt` typed as str. */
static void
handle_end_menu(va_list ap, void *ret_ptr)
{
    int w = va_arg(ap, int);
    const char *prompt = va_arg(ap, const char *);
    (void) ret_ptr;

    unity_emit_event_begin("end_menu");
    unity_emit_kv_int("w", w);
    if (prompt)
        unity_emit_kv_str("prompt", prompt);
    unity_emit_kv_int("menu_id", s_current_menu_id);
    unity_emit_event_end();
}

/* shim_select_menu fmt is "iiip": winid, int how, MENU_ITEM_P **menu_list.
 * Returns int — count of items picked, 0 for "no pick" (engine often
 * loops on this for PICK_ANY), or -1 for cancel.
 *
 * We emit the prompt, block on the input queue, and on a positive pick
 * count allocate a MENU_ITEM_P array sized to fit. The engine takes
 * ownership and frees it via a plain free(). union any.a_int64 carries
 * the id we sent on add_menu — on x64 little-endian the lower 4 bytes
 * are also a_int and the lower 8 bytes are a_void, so fields the engine
 * reads via either accessor see the same payload. */
static void
handle_select_menu(va_list ap, void *ret_ptr)
{
    int w = va_arg(ap, int);
    int how = va_arg(ap, int);
    MENU_ITEM_P **menu_list = va_arg(ap, MENU_ITEM_P **);
    struct unity_menu_pick picks[256];
    int n;

    unity_emit_event_begin("select_menu");
    unity_emit_kv_int("w", w);
    unity_emit_kv_int("how", how);
    unity_emit_kv_int("menu_id", s_current_menu_id);
    unity_emit_event_end();
    unity_emit_flush();

    n = unity_input_queue_pop_menu_pick(picks, 256, s_current_menu_id);

    if (menu_list)
        *menu_list = NULL;
    if (n > 0 && menu_list) {
        MENU_ITEM_P *items =
            (MENU_ITEM_P *) malloc((size_t) n * sizeof *items);
        if (items) {
            int i;
            for (i = 0; i < n; i++) {
                items[i].item.a_int64 = picks[i].id;
                items[i].count = picks[i].count;
                items[i].itemflags = 0;
            }
            *menu_list = items;
        } else {
            n = 0;
        }
    }
    if (ret_ptr)
        *(int *) ret_ptr = n;
}

/* shim_message_menu fmt is "ciis": char let, int how, const char *mesg.
 * Returns char (the keystroke that picked an item, or '\033' to cancel).
 * Same shape as yn_function — emit prompt, block on key queue, return. */
static void
handle_message_menu(va_list ap, void *ret_ptr)
{
    int let = va_arg(ap, int);  /* char promoted */
    int how = va_arg(ap, int);
    const char *mesg = va_arg(ap, const char *);
    char let_buf[2];

    let_buf[0] = (char) let;
    let_buf[1] = '\0';

    unity_emit_event_begin("message_menu");
    unity_emit_kv_str("let", let_buf);
    unity_emit_kv_int("how", how);
    unity_emit_kv_str("mesg", mesg);
    unity_emit_event_end();
    unity_emit_flush();

    if (ret_ptr) {
        int k = unity_input_queue_pop_key();
        *(char *) ret_ptr = (char) ((k < 0) ? '\033' : k);
    }
}

/* shim_getlin fmt is "vsp": const char *query, char *bufp.
 * The engine passes a buffer it owns (BUFSZ chars per global.h). We
 * emit the prompt, then block on the input queue's text slot and copy
 * the reply into bufp. NetHack convention: bufp[0]='\033' indicates the
 * user cancelled. */
static void
handle_getlin(va_list ap, void *ret_ptr)
{
    const char *query = va_arg(ap, const char *);
    char *bufp = va_arg(ap, char *);
    (void) ret_ptr;

    unity_emit_event_begin("getlin");
    unity_emit_kv_str("query", query);
    unity_emit_event_end();
    unity_emit_flush();

    if (bufp)
        unity_input_queue_pop_text(bufp, BUFSZ);
}

/* shim_nh_poskey fmt is "ippp": coordxy *x, coordxy *y, int *mod.
 * Returns int (key). The engine inspects all four to distinguish a
 * keypress (key != 0) from a position click (key == 0, x/y/mod set).
 * Harness sends `answer_pos` with any subset of {x, y, mod, key}. */
static void
handle_nh_poskey(va_list ap, void *ret_ptr)
{
    coordxy *xp = va_arg(ap, coordxy *);
    coordxy *yp = va_arg(ap, coordxy *);
    int *modp = va_arg(ap, int *);
    int x = -1, y = -1, mod = 0;
    int key;

    unity_emit_event_begin("nh_poskey");
    unity_emit_event_end();
    unity_emit_flush();

    key = unity_input_queue_pop_poskey(&x, &y, &mod);

    if (xp) *xp = (coordxy) x;
    if (yp) *yp = (coordxy) y;
    if (modp) *modp = mod;
    if (ret_ptr)
        *(int *) ret_ptr = key;
}

/* Emit the table of extended commands the player may invoke, as a
 * `commands` object mapping each command name to its index:
 *   "commands": { "quit": 17, "pray": 42, ... }
 * The index is exactly the position doextcmd() indexes back into
 * (src/cmd.c:504 `extcmdlist[idx]`), so a returned answer_ext_cmd.index
 * round-trips directly with no separate lookup table on the engine side.
 *
 * Visibility mirrors what tty's extcmds_match() would offer when the
 * player types a full command name: skip non-functional and internal
 * entries, and skip wizard-mode commands outside wizard mode. We do NOT
 * require AUTOCOMPLETE — that flag only governs tty tab-completion
 * suggestions; any visible command can still be typed out in full. */
static void
emit_ext_cmd_list(void)
{
    int i;

    unity_emit_kv_obj_begin("commands");
    for (i = 0; extcmdlist[i].ef_txt != NULL; i++) {
        unsigned f = extcmdlist[i].flags;

        if (f & (CMD_NOT_AVAILABLE | INTERNALCMD))
            continue;
        if ((f & WIZMODECMD) && !wizard)
            continue;

        unity_emit_kv_int(extcmdlist[i].ef_txt, i);
    }
    unity_emit_kv_obj_end();
}

/* shim_get_ext_cmd fmt is "iv": no args, returns int. We enrich the event
 * with the `commands` table so the bridge can resolve a typed command
 * name to the index this call expects back. The harness sends
 * `answer_ext_cmd` with `index` set to the ext-cmd table position; -1
 * means the user cancelled. */
static void
handle_get_ext_cmd(va_list ap, void *ret_ptr)
{
    (void) ap;
    unity_emit_event_begin("get_ext_cmd");
    emit_ext_cmd_list();
    unity_emit_event_end();
    unity_emit_flush();
    if (ret_ptr)
        *(int *) ret_ptr = unity_input_queue_pop_ext_cmd();
}

/* ---- dispatch table ---- */

typedef void (*shim_handler_t)(va_list ap, void *ret_ptr);

static const struct {
    const char     *name;     /* shim_* name including the prefix */
    shim_handler_t  handler;
} dispatch_table[] = {
    { "shim_print_glyph",        handle_print_glyph },
    { "shim_putstr",             handle_putstr },
    { "shim_raw_print",          handle_raw_print },
    { "shim_raw_print_bold",     handle_raw_print_bold },
    { "shim_status_init",        handle_status_init },
    { "shim_status_enablefield", handle_status_enablefield },
    { "shim_status_update",      handle_status_update },
    { "shim_init_nhwindows",     handle_init_nhwindows },
    { "shim_create_nhwindow",    handle_create_nhwindow },
    { "shim_clear_nhwindow",     handle_clear_nhwindow },
    { "shim_display_nhwindow",   handle_display_nhwindow },
    { "shim_destroy_nhwindow",   handle_destroy_nhwindow },
    { "shim_exit_nhwindows",     handle_exit_nhwindows },
    { "shim_suspend_nhwindows",  handle_suspend_nhwindows },
    { "shim_resume_nhwindows",   handle_resume_nhwindows },
    { "shim_curs",               handle_curs },
    { "shim_cliparound",         handle_cliparound },
    { "shim_askname",            handle_askname },
    { "shim_player_selection",   handle_player_selection },
    { "shim_update_inventory",   handle_update_inventory },
    { "shim_nhgetch",            handle_nhgetch },
    { "shim_yn_function",        handle_yn_function },
    { "shim_getlin",             handle_getlin },
    { "shim_nh_poskey",          handle_nh_poskey },
    { "shim_get_ext_cmd",        handle_get_ext_cmd },
    { "shim_start_menu",         handle_start_menu },
    { "shim_add_menu",           handle_add_menu },
    { "shim_end_menu",           handle_end_menu },
    { "shim_select_menu",        handle_select_menu },
    { "shim_message_menu",       handle_message_menu },
    { NULL, NULL }
};

void
unity_shim_callback(const char *name, void *ret_ptr, const char *fmt, ...)
{
    int i;

    if (!name)
        return;

    for (i = 0; dispatch_table[i].name; i++) {
        if (strcmp(name, dispatch_table[i].name) == 0) {
            va_list ap;
            va_start(ap, fmt);
            dispatch_table[i].handler(ap, ret_ptr);
            va_end(ap);
            return;
        }
    }

    /* Legacy fallback: emit just name + format string so the harness
     * still sees the call. Migrate to a typed handler when needed. */
    unity_emit_callback(name, fmt);
}
