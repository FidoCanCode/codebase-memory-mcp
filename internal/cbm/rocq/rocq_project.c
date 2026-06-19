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
    // physdir/logical are NULL only when their dup_range allocation failed (OOM);
    // add_dune never passes an empty logical (it requires a non-empty name atom).
    if (!physdir || !logical) {
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
    int nlen = (int)strlen(needle); // always a non-empty literal ("coq.theory"/"name")
    if (hlen < nlen) {
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
            // dir is always a non-null directory string (the caller's responsibility).
            projmap_add(m, dup_range(dir, (int)strlen(dir)), dup_range(text + start, na - start));
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
        const char *lp = m->entries[i].logical; // non-empty (projmap_add rejects empty)
        size_t ll = strlen(lp);
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
    return n < outsz; // n is always >= 2 (the ".v" suffix), so only truncation can fail
}

bool rocq_projmap_logical_for_path(const RocqProjMap *m, const char *rel_path, char *out,
                                   int outsz) {
    if (!m || !rel_path || outsz <= 0) {
        return false;
    }
    // Most specific (longest physdir) theory root that contains the path.
    int best = -1;
    size_t best_len = 0;
    size_t rl = strlen(rel_path);
    for (int i = 0; i < m->count; i++) {
        const char *pd = m->entries[i].physdir;
        size_t pl = strlen(pd);
        bool match = (pl == 0) || (rl > pl && strncmp(rel_path, pd, pl) == 0 && rel_path[pl] == '/');
        if (match && (best < 0 || pl > best_len)) {
            best = i;
            best_len = pl;
        }
    }
    if (best < 0) {
        return false;
    }
    const RocqProjEntry *e = &m->entries[best];
    size_t pl = strlen(e->physdir);
    const char *under = rel_path + (pl ? pl + 1 : 0); // path beneath the theory root
    size_t ul = strlen(under);
    if (ul >= 2 && under[ul - 2] == '.' && under[ul - 1] == 'v') {
        ul -= 2; // strip ".v"
    }

    // logical is always non-empty (projmap_add rejects empty names), so n >= 1
    // after this and the '.' separator below only needs a truncation check.
    int n = (int)snprintf(out, (size_t)outsz, "%s", e->logical);
    if (ul > 0) {
        if (n < outsz) {
            out[n++] = '.';
        }
        for (size_t k = 0; k < ul && n < outsz - 1; k++) {
            out[n++] = (under[k] == '/') ? '.' : under[k];
        }
    }
    if (n < outsz) {
        out[n] = '\0';
    }
    return n < outsz;
}
