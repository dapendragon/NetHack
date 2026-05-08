/* Yendor RNG-seed override.
 *
 * Lets the harness pass `--seed N` on the command line so save/restore
 * and movement tests get a deterministic dungeon + spawn tile. Without
 * an override the engine's normal `sys_random_seed()` path runs (BCrypt
 * via windsys.c). With one, every call to `sys_random_seed()` returns
 * the same fixed value — which is fine because NetHack invokes it
 * exactly twice during init (rn2 and rn2_on_display_rng, in
 * initoptions()). Reseed-on-game-events isn't a concern: NetHack only
 * reseeds when `has_strong_rngseed` is set, and the BCrypt path is
 * what sets that — our override path leaves it FALSE.
 *
 * Kept free of hack.h: the override is queried from the engine's
 * port glue (windsys.c sys_random_seed), which already pulls in
 * hack.h via its own translation unit. We just need plain C types
 * here.
 */

static unsigned long s_seed;
static int           s_has_override;

void
unity_seed_set(unsigned long seed)
{
    s_seed = seed;
    s_has_override = 1;
}

int
unity_seed_has_override(void)
{
    return s_has_override;
}

unsigned long
unity_seed_get(void)
{
    return s_seed;
}
