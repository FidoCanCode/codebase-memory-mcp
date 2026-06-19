// SPDX-License-Identifier: MIT
//
// rocq_parse.h — Vernacular → TSTree builder for Rocq.
//
// Original work. Drives rocq_lex over a .v source and constructs a real
// tree-sitter TSTree (via rocq_tree's builder) that mirrors Rocq's command
// structure: definitions/theorems, inductives + constructors, records/classes +
// fields, modules/sections as *containers* (so nesting is reflected in the
// tree), requires, notations, coercions, and `term`/`proof` nodes holding the
// reference tokens. The parser is deliberately *syntactic only* — it records
// structure, not meaning. All semantics (qualified-name nesting, notation
// resolution, proof-dependency calls, typeclass/coercion edges) live in the
// consumer that walks the finished tree (rocq_walk.c), exactly as the Hybrid LSP
// modules walk a TSTree. Rocq's grammar is user-extensible at parse time, so the
// dynamic parts are resolved by that walk in file order, not baked into a static
// table.
#ifndef CBM_ROCQ_PARSE_H
#define CBM_ROCQ_PARSE_H

// Parse `source` and return a freshly constructed tree-sitter TSTree (as void*).
// The caller owns it and must free it with rocq_tree_free (rocq_tree.h). Returns
// NULL only on allocation failure.
void *rocq_parse_to_tree(const char *source, int source_len);

#endif // CBM_ROCQ_PARSE_H
