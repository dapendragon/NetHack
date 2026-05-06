/* --sandbox <dir> handling. Points every fqn_prefix[] slot at the
 * supplied directory and locks it, so all engine file I/O — saves, levels,
 * bones, scores, locks, configs, dlb data — lives under that one tree.
 *
 * No registry reads, no %APPDATA%, no env lookups. POSIX-clean: works on
 * Linux/macOS later in Phase 1 (M9) without changes. */

#include "hack.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Scan argv for `--sandbox <dir>` and remove both tokens from argv in
 * place. Returns the directory string (still owned by argv's storage),
 * or NULL if the flag wasn't present.
 * Must be called before argv is handed to early_init(). */
const char *
unity_paths_extract_sandbox(int *argc, char *argv[])
{
    int i, j;
    const char *dir = NULL;

    for (i = 1; i + 1 < *argc; i++) {
        if (strcmp(argv[i], "--sandbox") == 0) {
            dir = argv[i + 1];
            for (j = i; j + 2 <= *argc; j++)
                argv[j] = argv[j + 2];
            *argc -= 2;
            break;
        }
    }
    return dir;
}

/* Duplicate `s` with a trailing path separator. Returns malloc'd memory
 * the engine will treat as fqn_prefix[] storage. */
static char *
dup_with_trailing_sep(const char *s)
{
    size_t len = strlen(s);
    int needs_sep = (len == 0
                     || (s[len - 1] != '\\' && s[len - 1] != '/'));
    size_t n = len + (needs_sep ? 1 : 0) + 1;
    char *p = (char *) malloc(n);
    if (!p)
        return NULL;
    memcpy(p, s, len);
    if (needs_sep)
        p[len++] = '\\';
    p[len] = '\0';
    return p;
}

/* Point every prefix slot at <dir> and lock. Idempotent: replaces any
 * previous value (typically the defaults from set_default_prefix_locations). */
void
unity_paths_set_sandbox(const char *dir)
{
    int i;
    char *base;

    if (!dir || !*dir)
        return;

    base = dup_with_trailing_sep(dir);
    if (!base)
        return;

    for (i = 0; i < PREFIX_COUNT; i++) {
        char *copy = (char *) malloc(strlen(base) + 1);
        if (copy) {
            strcpy(copy, base);
            /* Engine memory is leak-tolerant for these; the existing
             * pointer was either a static literal or also malloc'd by
             * windsys.c, but freeing it crosses an allocator boundary
             * and isn't safe. Leave the prior value to leak. */
            gf.fqn_prefix[i] = copy;
        }
        fqn_prefix_locked[i] = TRUE;
    }
    free(base);
}
