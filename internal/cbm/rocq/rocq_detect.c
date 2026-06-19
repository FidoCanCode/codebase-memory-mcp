// SPDX-License-Identifier: MIT
//
// rocq_detect.c — see rocq_detect.h. Disambiguates Rocq from Verilog for the
// shared .v extension by running the real Vernacular parser over a bounded
// prefix and counting recognized declarations, instead of keyword sniffing.
#include "rocq/rocq_detect.h"

#include "rocq/rocq_cst.h"
#include "rocq/rocq_parse.h"
#include "rocq/rocq_tree.h"
#include "tree_sitter/api.h"

// Symbols that represent a genuine top-level Vernacular declaration/command —
// positive evidence the text is Rocq. Leaf tokens, comments, terms, proofs, and
// constructor/field sub-parts are excluded; unrecognized commands produce no
// node at all (the parser skips them to the next command terminator).
static bool is_decl_symbol(TSSymbol s) {
    switch (s) {
    case RSYM_DEFINITION:
    case RSYM_THEOREM:
    case RSYM_INDUCTIVE:
    case RSYM_RECORD:
    case RSYM_CLASS:
    case RSYM_INSTANCE:
    case RSYM_MODULE:
    case RSYM_MODULE_TYPE:
    case RSYM_SECTION:
    case RSYM_REQUIRE:
    case RSYM_FROM_REQUIRE:
    case RSYM_NOTATION:
    case RSYM_TACTIC:
    case RSYM_ASSUMPTION:
    case RSYM_COERCION:
        return true;
    default:
        return false;
    }
}

int cbm_rocq_count_decls(const char *src, int len) {
    if (!src || len <= 0) {
        return 0;
    }
    void *tree = rocq_parse_to_tree(src, len);
    if (!tree) {
        return 0;
    }

    // Single O(n) depth-first walk over the (small, bounded) tree via a cursor —
    // ts_node_child(n, i) is O(i), so a cursor avoids accidental O(n^2).
    TSNode root = ts_tree_root_node((TSTree *)tree);
    TSTreeCursor cur = ts_tree_cursor_new(root);
    int count = 0;
    if (is_decl_symbol(ts_node_symbol(root))) {
        count++;
    }
    for (;;) {
        if (ts_tree_cursor_goto_first_child(&cur)) {
            if (is_decl_symbol(ts_node_symbol(ts_tree_cursor_current_node(&cur)))) {
                count++;
            }
            continue;
        }
        bool advanced = false;
        for (;;) {
            if (ts_tree_cursor_goto_next_sibling(&cur)) {
                if (is_decl_symbol(ts_node_symbol(ts_tree_cursor_current_node(&cur)))) {
                    count++;
                }
                advanced = true;
                break;
            }
            if (!ts_tree_cursor_goto_parent(&cur)) {
                break;
            }
        }
        if (!advanced) {
            break;
        }
    }
    ts_tree_cursor_delete(&cur);
    rocq_tree_free(tree);
    return count;
}
