// SPDX-License-Identifier: MIT
//
// rocq_notation.c — see rocq_notation.h. Original cross-file notation support.
#include "rocq/rocq_notation.h"
#include "rocq/rocq_lex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- token helpers ---------------------------------------------------------

static bool teq(RocqToken t, const char *kw) {
    size_t n = strlen(kw);
    return t.kind == ROCQ_TOK_IDENT && (size_t)t.len == n && memcmp(t.text, kw, n) == 0;
}
static bool is_assign(RocqToken t) {
    return t.kind == ROCQ_TOK_SYMBOL && t.len >= 2 && t.text[0] == ':' && t.text[1] == '=';
}
// Punctuation too common to treat as a notation operator (kept in sync with the
// parser's own guard).
static bool op_too_common(const char *o, int l) {
    if (l != 1) {
        return false;
    }
    return o[0] == ';' || o[0] == ',' || o[0] == ':' || o[0] == '|';
}
static bool head_keyword(RocqToken t) {
    return teq(t, "fun") || teq(t, "forall") || teq(t, "match") || teq(t, "let") || teq(t, "fix");
}

// ---- scan a .v source ------------------------------------------------------

static void push_notation(CBMArena *a, RocqNotationEntry **arr, int *count, int *cap,
                          const char *op, int oplen, const char *target) {
    if (*count >= *cap) {
        int nc = *cap ? *cap * 2 : 8;
        RocqNotationEntry *n = (RocqNotationEntry *)cbm_arena_alloc(a, (size_t)nc * sizeof(*n));
        if (!n) {
            return;
        }
        if (*arr) { // *arr non-null at a grow implies *count > 0 (we have pushed before)
            memcpy(n, *arr, (size_t)*count * sizeof(*n));
        }
        *arr = n;
        *cap = nc;
    }
    (*arr)[*count].op = cbm_arena_strndup(a, op, (size_t)oplen);
    (*arr)[*count].target = target;
    (*count)++;
}

static void push_require(CBMArena *a, const char ***arr, int *count, int *cap, const char *s) {
    if (*count >= *cap) {
        int nc = *cap ? *cap * 2 : 8;
        const char **n = (const char **)cbm_arena_alloc(a, (size_t)nc * sizeof(*n));
        if (!n) {
            return;
        }
        if (*arr) { // *arr non-null at a grow implies *count > 0 (we have pushed before)
            memcpy(n, *arr, (size_t)*count * sizeof(*n));
        }
        *arr = n;
        *cap = nc;
    }
    (*arr)[(*count)++] = s;
}

// Handle a Notation/Infix command (keyword already consumed): record each
// literal operator of the pattern bound to the expansion's head identifier.
static void scan_notation(CBMArena *a, RocqLexer *lx, RocqNotationEntry **nots, int *nc, int *ncap) {
    RocqToken str = {0};
    bool have_str = false, seen_assign = false;
    const char *target = NULL;
    for (;;) {
        RocqToken u = rocq_lex_next(lx);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            break;
        }
        if (!have_str && u.kind == ROCQ_TOK_STRING && u.len >= 2) {
            str = u;
            have_str = true;
        } else if (is_assign(u)) {
            seen_assign = true;
        } else if (seen_assign && !target && u.kind == ROCQ_TOK_IDENT && !head_keyword(u)) {
            target = cbm_arena_strndup(a, u.text, (size_t)u.len);
        }
    }
    if (!have_str || !target) {
        return;
    }
    RocqLexer il;
    rocq_lex_init(&il, str.text + 1, str.len - 2);
    for (;;) {
        RocqToken p = rocq_lex_next(&il);
        if (p.kind == ROCQ_TOK_EOF) {
            break;
        }
        if (p.kind == ROCQ_TOK_SYMBOL && !op_too_common(p.text, p.len)) {
            push_notation(a, nots, nc, ncap, p.text, p.len, target);
        }
    }
}

// Record a required module; if `is_export`, also record it as re-exported.
static void add_require(CBMArena *a, RocqFileScan *out, int *rcap, int *ecap, const char *nm,
                        bool is_export) {
    push_require(a, &out->requires, &out->require_count, rcap, nm);
    if (is_export) {
        push_require(a, &out->exports, &out->export_count, ecap, nm);
    }
}

static void scan_require(CBMArena *a, RocqLexer *lx, RocqFileScan *out, int *rcap, int *ecap) {
    bool is_export = false;
    for (;;) {
        RocqToken u = rocq_lex_next(lx);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            return;
        }
        if (u.kind != ROCQ_TOK_IDENT) {
            continue;
        }
        if (teq(u, "Export")) {
            is_export = true;
            continue;
        }
        if (teq(u, "Import")) {
            continue;
        }
        add_require(a, out, rcap, ecap, cbm_arena_strndup(a, u.text, (size_t)u.len), is_export);
    }
}

static void scan_from(CBMArena *a, RocqLexer *lx, RocqFileScan *out, int *rcap, int *ecap) {
    RocqToken pfx = rocq_lex_next(lx);
    if (pfx.kind != ROCQ_TOK_IDENT) {
        while (pfx.kind != ROCQ_TOK_DOT && pfx.kind != ROCQ_TOK_EOF) {
            pfx = rocq_lex_next(lx);
        }
        return;
    }
    RocqToken t;
    for (;;) {
        t = rocq_lex_next(lx);
        if (t.kind == ROCQ_TOK_DOT || t.kind == ROCQ_TOK_EOF) {
            return;
        }
        if (teq(t, "Require")) {
            break;
        }
    }
    bool is_export = false;
    for (;;) {
        RocqToken u = rocq_lex_next(lx);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            return;
        }
        if (u.kind != ROCQ_TOK_IDENT) {
            continue;
        }
        if (teq(u, "Export")) {
            is_export = true;
            continue;
        }
        if (teq(u, "Import")) {
            continue;
        }
        char buf[512];
        // pfx and module are both non-empty identifiers, so n is always >= 3.
        int n = snprintf(buf, sizeof(buf), "%.*s.%.*s", pfx.len, pfx.text, u.len, u.text);
        add_require(a, out, rcap, ecap,
                    cbm_arena_strndup(a, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1)),
                    is_export);
    }
}

// Standalone `Import M.` / `Export M.` (opening already-required modules). An
// Export re-exports to importers; both make the named modules visible here.
static void scan_open(CBMArena *a, RocqLexer *lx, RocqFileScan *out, int *rcap, int *ecap,
                      bool is_export) {
    for (;;) {
        RocqToken u = rocq_lex_next(lx);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            return;
        }
        if (u.kind == ROCQ_TOK_IDENT) {
            add_require(a, out, rcap, ecap, cbm_arena_strndup(a, u.text, (size_t)u.len), is_export);
        }
    }
}

static void skip_to_dot(RocqLexer *lx) {
    for (;;) {
        RocqToken t = rocq_lex_next(lx);
        if (t.kind == ROCQ_TOK_DOT || t.kind == ROCQ_TOK_EOF) {
            return;
        }
    }
}

void rocq_scan_file(CBMArena *a, const char *src, int len, RocqFileScan *out) {
    out->notations = NULL;
    out->notation_count = 0;
    out->requires = NULL;
    out->require_count = 0;
    out->exports = NULL;
    out->export_count = 0;
    int ncap = 0, rcap = 0, ecap = 0;

    RocqLexer lx;
    rocq_lex_init(&lx, src, len);
    for (;;) {
        RocqToken t = rocq_lex_next(&lx);
        if (t.kind == ROCQ_TOK_EOF) {
            break;
        }
        if (t.kind == ROCQ_TOK_DOT) {
            continue;
        }
        if (t.kind == ROCQ_TOK_IDENT) {
            if (teq(t, "Notation") || teq(t, "Infix")) {
                scan_notation(a, &lx, &out->notations, &out->notation_count, &ncap);
                continue;
            }
            if (teq(t, "Reserved")) {
                RocqToken n = rocq_lex_next(&lx);
                if (teq(n, "Notation")) {
                    scan_notation(a, &lx, &out->notations, &out->notation_count, &ncap);
                } else if (n.kind != ROCQ_TOK_DOT && n.kind != ROCQ_TOK_EOF) {
                    skip_to_dot(&lx);
                }
                continue;
            }
            if (teq(t, "Require")) {
                scan_require(a, &lx, out, &rcap, &ecap);
                continue;
            }
            if (teq(t, "From")) {
                scan_from(a, &lx, out, &rcap, &ecap);
                continue;
            }
            if (teq(t, "Import")) {
                scan_open(a, &lx, out, &rcap, &ecap, false);
                continue;
            }
            if (teq(t, "Export")) {
                scan_open(a, &lx, out, &rcap, &ecap, true);
                continue;
            }
        }
        skip_to_dot(&lx); // unrelated command
    }
}

// ---- seed DB ---------------------------------------------------------------

typedef struct {
    char *rel_path;
    RocqNotationEntry *entries;
    int count;
    int cap;
} SeedBucket;

struct RocqSeedDB {
    SeedBucket *buckets;
    int count;
    int cap;
};

RocqSeedDB *rocq_seeddb_new(void) {
    RocqSeedDB *db = (RocqSeedDB *)calloc(1, sizeof(*db));
    return db;
}

void rocq_seeddb_free(RocqSeedDB *db) {
    if (!db) {
        return;
    }
    for (int i = 0; i < db->count; i++) {
        SeedBucket *b = &db->buckets[i];
        for (int j = 0; j < b->count; j++) {
            free((void *)b->entries[j].op);
            free((void *)b->entries[j].target);
        }
        free(b->entries);
        free(b->rel_path);
    }
    free(db->buckets);
    free(db);
}

static SeedBucket *seed_bucket(RocqSeedDB *db, const char *rel_path) {
    for (int i = 0; i < db->count; i++) {
        if (strcmp(db->buckets[i].rel_path, rel_path) == 0) {
            return &db->buckets[i];
        }
    }
    if (db->count >= db->cap) {
        int nc = db->cap ? db->cap * 2 : 16;
        SeedBucket *nb = (SeedBucket *)realloc(db->buckets, (size_t)nc * sizeof(*nb));
        if (!nb) {
            return NULL;
        }
        db->buckets = nb;
        db->cap = nc;
    }
    SeedBucket *b = &db->buckets[db->count++];
    b->rel_path = strdup(rel_path);
    b->entries = NULL;
    b->count = 0;
    b->cap = 0;
    return b;
}

void rocq_seeddb_add(RocqSeedDB *db, const char *rel_path, const char *op, const char *target) {
    if (!db || !rel_path || !op || !target) {
        return;
    }
    SeedBucket *b = seed_bucket(db, rel_path);
    if (!b) {
        return;
    }
    // de-dup identical (op,target) within a file's seed
    for (int j = 0; j < b->count; j++) {
        if (strcmp(b->entries[j].op, op) == 0 && strcmp(b->entries[j].target, target) == 0) {
            return;
        }
    }
    if (b->count >= b->cap) {
        int nc = b->cap ? b->cap * 2 : 8;
        RocqNotationEntry *ne = (RocqNotationEntry *)realloc(b->entries, (size_t)nc * sizeof(*ne));
        if (!ne) {
            return;
        }
        b->entries = ne;
        b->cap = nc;
    }
    b->entries[b->count].op = strdup(op);
    b->entries[b->count].target = strdup(target);
    b->count++;
}

int rocq_seeddb_lookup(const RocqSeedDB *db, const char *rel_path, const RocqNotationEntry **out) {
    if (!db || !rel_path) {
        return 0;
    }
    for (int i = 0; i < db->count; i++) {
        if (strcmp(db->buckets[i].rel_path, rel_path) == 0) {
            *out = db->buckets[i].entries;
            return db->buckets[i].count;
        }
    }
    return 0;
}

// ---- process-global --------------------------------------------------------

static const RocqSeedDB *g_seeddb = NULL;
void cbm_rocq_set_seeddb(const RocqSeedDB *db) { g_seeddb = db; }
const RocqSeedDB *cbm_rocq_get_seeddb(void) { return g_seeddb; }
