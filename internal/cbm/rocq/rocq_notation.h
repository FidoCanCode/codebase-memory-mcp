// SPDX-License-Identifier: MIT
//
// rocq_notation.h — cross-file notation support for Rocq.
//
// Original work. A notation declared in one module and `Require Import`ed into
// another must resolve at the importing file's use sites. This module scans a
// `.v` file for the notations it defines and the modules it requires, and holds
// a per-file "seed" table — the notations each file inherits from its imports —
// which the parser pre-loads before walking the file (own notations then layer
// on top, in file order).
#ifndef CBM_ROCQ_NOTATION_H
#define CBM_ROCQ_NOTATION_H

#include "arena.h"

typedef struct {
    const char *op;     // literal operator token of a notation pattern
    const char *target; // head identifier its RHS expands to
} RocqNotationEntry;

// Result of scanning one .v source. All pointers are arena-allocated.
typedef struct {
    RocqNotationEntry *notations; // notations the file DEFINES
    int notation_count;
    const char **requires; // logical module names the file Require's (e.g. "MyDev.Base")
    int require_count;
} RocqFileScan;

// Scan a .v source for its notation bindings and Require'd module names.
void rocq_scan_file(CBMArena *a, const char *src, int len, RocqFileScan *out);

// Per-file seed table: repo-relative path -> notation entries available to that
// file from its imports. malloc-backed; owns its copies.
typedef struct RocqSeedDB RocqSeedDB;
RocqSeedDB *rocq_seeddb_new(void);
void rocq_seeddb_free(RocqSeedDB *db);
void rocq_seeddb_add(RocqSeedDB *db, const char *rel_path, const char *op, const char *target);
// On match, sets *out to the file's entries and returns their count.
int rocq_seeddb_lookup(const RocqSeedDB *db, const char *rel_path, const RocqNotationEntry **out);

// Process-global seed DB: set by the pipeline pre-pass, read by the parser.
void cbm_rocq_set_seeddb(const RocqSeedDB *db);
const RocqSeedDB *cbm_rocq_get_seeddb(void);

#endif // CBM_ROCQ_NOTATION_H
