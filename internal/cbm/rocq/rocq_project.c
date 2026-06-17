// SPDX-License-Identifier: MIT
//
// rocq_project.c — see rocq_project.h. Original dune coq.theory resolver.
#include "rocq/rocq_project.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rocq_projmap_init(RocqProjMap *m) {
    m->entries = NULL;
    m->count = 0;
    m->cap = 0;
}

void rocq_projmap_free(RocqProjMap *m) {
    if (!m) {
        return;
    }
    for (int i = 0; i < m->count; i++) {
        free(m->entries[i].physdir);
        free(m->entries[i].logical);
    }
    free(m->entries);
    m->entries = NULL;
    m->count = 0;
    m->cap = 0;
}

// Take ownership of physdir/logical; drops them if the map can't grow.
static void projmap_add(RocqProjMap *m, char *physdir, char *logical) {
    if (!physdir || !logical || !logical[0]) {
        free(physdir);
        free(logical);
        return;
    }
    if (m->count >= m->cap) {
        int newcap = m->cap == 0 ? 8 : m->cap * 2;
        RocqProjEntry *ne = realloc(m->entries, (size_t)newcap * sizeof(*ne));
        if (!ne) {
            free(physdir);
            free(logical);
            return;
        }
        m->entries = ne;
        m->cap = newcap;
    }
    m->entries[m->count].physdir = physdir;
    m->entries[m->count].logical = logical;
    m->count++;
}

static char *dup_range(const char *s, int len) {
    char *out = malloc((size_t)len + 1);
    if (out) {
        memcpy(out, s, (size_t)len);
        out[len] = '\0';
    }
    return out;
}

// Index of the first occurrence of `needle` within hay[0..hlen), or -1.
static int find_sub(const char *hay, int hlen, const char *needle) {
    int nlen = (int)strlen(needle);
    if (nlen == 0 || hlen < nlen) {
        return -1;
    }
    for (int i = 0; i <= hlen - nlen; i++) {
        if (memcmp(hay + i, needle, (size_t)nlen) == 0) {
            return i;
        }
    }
    return -1;
}

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

void rocq_projmap_add_dune(RocqProjMap *m, const char *dir, const char *text, int len) {
    const char *KEY = "coq.theory";
    const int KLEN = 10;
    int i = 0;
    while (i + KLEN <= len) {
        int pos = find_sub(text + i, len - i, KEY);
        if (pos < 0) {
            break;
        }
        int after = i + pos + KLEN;
        // Find the `name` field of this stanza and take its atom as the logical
        // theory name (e.g. "(name MyDev)" → "MyDev", "(name A.B)" → "A.B").
        int npos = find_sub(text + after, len - after, "name");
        if (npos < 0) {
            break;
        }
        int na = after + npos + 4; // past "name"
        while (na < len && is_ws(text[na])) {
            na++;
        }
        int start = na;
        while (na < len && !is_ws(text[na]) && text[na] != ')' && text[na] != '(') {
            na++;
        }
        if (na > start) {
            projmap_add(m, dup_range(dir ? dir : "", (int)strlen(dir ? dir : "")),
                        dup_range(text + start, na - start));
        }
        i = (na > after) ? na : after; // always advance past this stanza's keyword
    }
}

bool rocq_projmap_resolve(const RocqProjMap *m, const char *logical, char *out, int outsz) {
    if (!m || !logical || !logical[0] || outsz <= 0) {
        return false;
    }
    // Prefer the longest matching logical prefix (most specific theory).
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < m->count; i++) {
        const char *lp = m->entries[i].logical;
        size_t ll = strlen(lp);
        if (ll == 0) {
            continue;
        }
        if (strncmp(logical, lp, ll) == 0 && (logical[ll] == '.' || logical[ll] == '\0')) {
            if (best < 0 || ll > best_len) {
                best = i;
                best_len = ll;
            }
        }
    }
    if (best < 0) {
        return false;
    }

    const RocqProjEntry *e = &m->entries[best];
    const char *rest = logical + strlen(e->logical);
    if (*rest == '.') {
        rest++;
    }

    int n = 0;
    if (e->physdir[0]) {
        n += snprintf(out + n, (size_t)(outsz - n), "%s", e->physdir);
    }
    if (*rest) {
        if (n > 0 && n < outsz) {
            out[n++] = '/';
        }
        for (const char *c = rest; *c && n < outsz - 1; c++) {
            out[n++] = (*c == '.') ? '/' : *c;
        }
    }
    n += snprintf(out + n, (size_t)(outsz - n > 0 ? outsz - n : 0), ".v");
    return n > 0 && n < outsz;
}
