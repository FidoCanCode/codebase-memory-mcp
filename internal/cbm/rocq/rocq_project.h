// SPDX-License-Identifier: MIT
//
// rocq_project.h — dune-based logical→physical module resolution for Rocq.
//
// Original work. Modern Rocq projects declare their load path with dune
// `(coq.theory (name L) ...)` stanzas: every `.v` file under the stanza's
// directory belongs to logical theory `L`, named by its path relative to that
// directory (e.g. `theories/Sub/Mod.v` → `L.Sub.Mod`). This module parses those
// stanzas and resolves a logical dotted module name to the physical `.v` path —
// the Rocq analogue of include-path resolution in the other language
// front-ends, and what disambiguates same-named modules across directories.
#ifndef CBM_ROCQ_PROJECT_H
#define CBM_ROCQ_PROJECT_H

#include <stdbool.h>

typedef struct {
    char *physdir; // repo-relative directory of the dune file ("" == repo root)
    char *logical; // theory logical name from (coq.theory (name ...))
} RocqProjEntry;

typedef struct RocqProjMap {
    RocqProjEntry *entries;
    int count;
    int cap;
} RocqProjMap;

void rocq_projmap_init(RocqProjMap *m);
void rocq_projmap_free(RocqProjMap *m);

// Parse a dune / dune-project file's text, adding a mapping for each
// `(coq.theory (name L) ...)` stanza found. `dir` is the repo-relative
// directory containing the file (NULL/"" for repo root) — the theory root.
void rocq_projmap_add_dune(RocqProjMap *m, const char *dir, const char *text, int len);

// Resolve a logical dotted module name (e.g. "MyDev.Base") to a repo-relative
// `.v` path (e.g. "theories/Base.v"). On a match, writes the path into `out`
// (capacity outsz) and returns true; otherwise returns false.
bool rocq_projmap_resolve(const RocqProjMap *m, const char *logical, char *out, int outsz);

#endif // CBM_ROCQ_PROJECT_H
