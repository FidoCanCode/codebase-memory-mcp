// SPDX-License-Identifier: MIT
//
// rocq_walk.h — extract the Rocq knowledge graph by walking a built TSTree.
//
// Original work. Consumes the tree-sitter TSTree produced by rocq_parse_to_tree
// using only the public ts_node_* API — the same surface the Hybrid LSP modules
// walk — and appends definitions/imports/calls/impl-traits to a CBMFileResult.
// This is where Rocq's *meaning* lives: qualified-name nesting through
// modules/sections, file-order notation resolution (the dynamic, user-extensible
// part of the grammar), proof-dependency calls, and typeclass/coercion edges.
#ifndef CBM_ROCQ_WALK_H
#define CBM_ROCQ_WALK_H

#include "cbm.h"

// Walk `tree` (a TSTree* passed as void*, from rocq_parse_to_tree) and append
// extracted graph data to `result`.
//   module_qn — the file's base qualified name (basis for all child QNs)
//   rel_path  — repo-relative path stored on each definition
//   source    — the original bytes, used to read node text
void rocq_walk_tree(CBMArena *a, CBMFileResult *result, const void *tree, const char *source,
                    const char *module_qn, const char *rel_path);

#endif // CBM_ROCQ_WALK_H
