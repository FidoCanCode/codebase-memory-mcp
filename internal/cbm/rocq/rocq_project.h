// SPDX-License-Identifier: MIT
//
// rocq_project.h — Logical-to-physical module resolution for Rocq projects.
//
// Original work. Rocq maps logical (dotted) module names onto physical .v
// files through `-Q <dir> <logical>` and `-R <dir> <logical>` directives in a
// _CoqProject file (and equivalently through dune coq.theory stanzas). This
// module parses those directives and resolves a logical name such as
// "MyDev.Base" to a repo-relative path such as "theories/Base.v" — the Rocq
// analogue of include-path resolution in the other language front-ends.
#ifndef CBM_ROCQ_PROJECT_H
#define CBM_ROCQ_PROJECT_H

#include <stdbool.h>

typedef struct {
    char *physdir; // repo-relative physical directory ("" == repo root)
    char *logical; // logical dotted prefix ("" == no prefix / path is the name)
    bool recursive;
} RocqProjEntry;

typedef struct {
    RocqProjEntry *entries;
    int count;
    int cap;
} RocqProjMap;

void rocq_projmap_init(RocqProjMap *m);
void rocq_projmap_free(RocqProjMap *m);

// Parse a _CoqProject file's text, adding its -Q/-R mappings. `dir` is the
// repo-relative directory containing the file (NULL/"" for repo root); it is
// prepended to each directive's physical directory.
void rocq_projmap_add_coqproject(RocqProjMap *m, const char *dir, const char *text, int len);

// Resolve a logical dotted module name to a repo-relative .v path. On a match,
// writes the path into out (capacity outsz) and returns true.
bool rocq_projmap_resolve(const RocqProjMap *m, const char *logical, char *out, int outsz);

#endif // CBM_ROCQ_PROJECT_H
