// SPDX-License-Identifier: MIT
//
// rocq_tree.h — tree-builder API: construct a tree-sitter TSTree directly from
// the Rocq parser, with no generated parse tables.
//
// The parser drives this builder (open a node, push leaves/child nodes, close)
// and the builder constructs real tree-sitter Subtrees with correct byte/point
// accounting, finishing with a TSTree the standard extractors + LSP can walk.
// TSTree is returned as void* so the parser TU need not pull in runtime headers.
#ifndef CBM_ROCQ_TREE_H
#define CBM_ROCQ_TREE_H

#include "rocq/rocq_cst.h"
#include <stdint.h>

typedef struct RocqTreeBuilder RocqTreeBuilder;

// Begin building over `source`. An implicit source_file node is opened.
RocqTreeBuilder *rocq_tb_new(const char *source, int source_len);

// Push a terminal token spanning [start_byte, end_byte) into the current node.
void rocq_tb_leaf(RocqTreeBuilder *b, RocqSymbol sym, uint32_t start_byte, uint32_t end_byte);

// Open a child node (subsequent leaves/nodes nest under it until closed).
void rocq_tb_open(RocqTreeBuilder *b, RocqSymbol sym, RocqProduction prod);

// Close the most recently opened node, attaching it to its parent.
void rocq_tb_close(RocqTreeBuilder *b);

// Finish: close the implicit source_file and return the constructed TSTree*
// (as void*). The caller owns it (free via the tree-sitter runtime).
void *rocq_tb_finish(RocqTreeBuilder *b);

// Free the builder's scratch state (call after rocq_tb_finish; does not free the
// returned tree).
void rocq_tb_free(RocqTreeBuilder *b);

// Free a TSTree* returned by rocq_tb_finish (wraps ts_tree_delete so callers need
// not pull in the runtime headers).
void rocq_tree_free(void *tree);

#endif // CBM_ROCQ_TREE_H
