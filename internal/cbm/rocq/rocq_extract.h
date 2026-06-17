// SPDX-License-Identifier: MIT
//
// rocq_extract.h — Entry point that drives the original Rocq front-end.
//
// Routed to from cbm_extract_file() for CBM_LANG_ROCQ instead of the
// tree-sitter pipeline. It owns computing the module QN, emitting the per-file
// module node, and invoking the Vernacular parser to populate the result's
// definitions, imports, and calls.
#ifndef CBM_ROCQ_EXTRACT_H
#define CBM_ROCQ_EXTRACT_H

#include "cbm.h"

// Extract a Rocq (.v) source file into `result` (which already has an
// initialized arena). Fills result->module_qn, result->is_test_file, and the
// defs/imports/calls arrays.
void cbm_rocq_extract_file(CBMArena *a, CBMFileResult *result, const char *source, int source_len,
                           const char *project, const char *rel_path);

#endif // CBM_ROCQ_EXTRACT_H
