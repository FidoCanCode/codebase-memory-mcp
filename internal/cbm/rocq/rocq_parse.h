// SPDX-License-Identifier: MIT
//
// rocq_parse.h — Vernacular command parser for Rocq.
//
// Original work. Drives rocq_lex over a .v source and fills a CBMFileResult's
// definitions, imports, and calls. It models the *command* layer (Definition,
// Theorem, Inductive, Module, Require, ...) plus enough of proof bodies to
// recover the proof-dependency graph (which lemmas/definitions a proof uses).
// It deliberately does not attempt to parse Gallina terms, tactic languages,
// or user notations — Rocq's grammar is extensible at parse time, so a
// faithful term parser is impossible without the live notation table.
#ifndef CBM_ROCQ_PARSE_H
#define CBM_ROCQ_PARSE_H

#include "cbm.h"

// Parse `source` and append definitions/imports/calls to `result`.
//   module_qn — the file's module qualified name (basis for all child QNs)
//   rel_path  — the file's repo-relative path (stored on each definition)
void rocq_parse_file(CBMArena *a, CBMFileResult *result, const char *source, int source_len,
                     const char *module_qn, const char *rel_path);

#endif // CBM_ROCQ_PARSE_H
