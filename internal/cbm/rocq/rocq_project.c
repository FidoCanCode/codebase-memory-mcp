// SPDX-License-Identifier: MIT
//
// rocq_project.c — see rocq_project.h. Original _CoqProject resolver.
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
    for (int i = 0; i < m->count; i++) {
        free(m->entries[i].physdir);
        free(m->entries[i].logical);
    }
    free(m->entries);
    m->entries = NULL;
    m->count = 0;
    m->cap = 0;
}

// Join a base directory and a directive-relative directory into a clean
// repo-relative path. A "." component collapses to the base. Returns a malloc'd
// string the caller owns.
static char *join_dir(const char *base, const char *rel) {
    bool base_empty = !base || base[0] == '\0' || (base[0] == '.' && base[1] == '\0');
    bool rel_dot = !rel || rel[0] == '\0' || (rel[0] == '.' && rel[1] == '\0');
    if (rel_dot) {
        return strdup(base_empty ? "" : base);
    }
    if (base_empty) {
        return strdup(rel);
    }
    size_t n = strlen(base) + 1 + strlen(rel) + 1;
    char *out = malloc(n);
    if (out) {
        snprintf(out, n, "%s/%s", base, rel);
    }
    return out;
}

static void projmap_add(RocqProjMap *m, char *physdir, char *logical, bool recursive) {
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
    m->entries[m->count].recursive = recursive;
    m->count++;
}

// Copy the whitespace-delimited token starting at *p into a fresh string and
// advance *p past it. Returns NULL at end of input.
static char *take_token(const char **p, const char *end) {
    const char *s = *p;
    while (s < end && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) {
        s++;
    }
    if (s >= end) {
        *p = s;
        return NULL;
    }
    const char *tok = s;
    while (s < end && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') {
        s++;
    }
    size_t len = (size_t)(s - tok);
    char *out = malloc(len + 1);
    if (out) {
        memcpy(out, tok, len);
        out[len] = '\0';
    }
    *p = s;
    return out;
}

void rocq_projmap_add_coqproject(RocqProjMap *m, const char *dir, const char *text, int len) {
    const char *p = text;
    const char *end = text + len;
    for (;;) {
        char *tok = take_token(&p, end);
        if (!tok) {
            break;
        }
        bool is_q = strcmp(tok, "-Q") == 0;
        bool is_r = strcmp(tok, "-R") == 0;
        if (is_q || is_r) {
            free(tok);
            char *physrel = take_token(&p, end);
            char *logical = take_token(&p, end);
            if (physrel && logical) {
                char *physdir = join_dir(dir, physrel);
                projmap_add(m, physdir, logical, is_r); // takes ownership of logical
                logical = NULL;
            }
            free(physrel);
            free(logical);
            continue;
        }
        free(tok); // ignore file names and other flags
    }
}

bool rocq_projmap_resolve(const RocqProjMap *m, const char *logical, char *out, int outsz) {
    if (!logical || !logical[0] || outsz <= 0) {
        return false;
    }
    // Prefer the longest matching logical prefix (most specific mapping).
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < m->count; i++) {
        const char *lp = m->entries[i].logical;
        size_t ll = strlen(lp);
        if (ll == 0) {
            if (best < 0) {
                best = i; // empty-prefix mapping is the lowest-priority fallback
            }
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

    // Build physdir + "/" + rest-with-dots-as-slashes + ".v".
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
