// SPDX-License-Identifier: MIT
//
// rocq_walk.c — see rocq_walk.h. Walks a built TSTree (public ts_node_* API) and
// reconstructs the Rocq knowledge graph, resolving the dynamic parts (notation
// operators) in document order as Rocq itself does.
#include "rocq/rocq_walk.h"
#include "rocq/rocq_cst.h"
#include "rocq/rocq_lex.h"
#include "rocq/rocq_notation.h"

#include "helpers.h"
#include "tree_sitter/api.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum { RW_QN_BUF = 1024 };

// A user notation's dynamic binding: a literal operator token from the pattern
// (e.g. "+") mapped to the head identifier its expansion targets (e.g. "Nat.add").
typedef struct {
    const char *op;     // arena-owned literal operator token
    const char *target; // arena-owned head identifier of the expansion
} RocqNotation;

typedef struct {
    CBMArena *a;
    CBMFileResult *result;
    const char *source;
    const char *module_qn;
    const char *rel_path; // arena-owned

    int last_def_idx; // most recent proof-bearing def (for `Proof.` attribution)

    RocqNotation *notations; // dynamic table, file-order
    int notation_count;
    int notation_cap;
} RW;

// ---- small helpers ---------------------------------------------------------

static int line_of(TSNode n) { return (int)ts_node_start_point(n).row + 1; }

// Every node passed here is a real token leaf (end > start); the only NULL this
// returns is from an arena allocation failure (handled by callers' `!name`).
static char *node_text(RW *rw, TSNode n) {
    uint32_t s = ts_node_start_byte(n);
    uint32_t e = ts_node_end_byte(n);
    return cbm_arena_strndup(rw->a, rw->source + s, (size_t)(e - s));
}

static const char *build_qn(RW *rw, const char *parent, const char *name) {
    char buf[RW_QN_BUF];
    snprintf(buf, sizeof(buf), "%s.%s", parent, name);
    return cbm_arena_strdup(rw->a, buf);
}

// The "name" field's leaf node (RPROD_NAME / RPROD_INSTANCE) — null if absent.
static TSNode field_name(TSNode n) { return ts_node_child_by_field_name(n, "name", 4); }

// Gallina / Ltac keywords that are never useful as proof-dependency targets.
// Over-collection is safe (the registry discards unknown names); the list only
// trims resolver work.
static bool is_filtered_callee(const char *t) {
    static const char *kw[] = {
        "forall", "fun",    "fix",     "cofix",  "match",  "with",     "end",
        "let",    "in",     "if",      "then",   "else",   "return",   "as",
        "at",     "by",     "using",   "Type",   "Prop",   "Set",      "of",
        "intro",  "intros", "apply",   "exact",  "refine", "rewrite",  "destruct",
        "induction", "simpl", "cbn",   "unfold", "fold",   "reflexivity", "symmetry",
        "transitivity", "assumption", "auto",  "eauto",  "trivial", "lia",   "nia",
        "omega",  "ring",   "field",   "congruence", "discriminate", "injection",
        "inversion", "subst", "clear", "generalize", "revert", "specialize", "pose",
        "set",    "remember", "assert", "cut",  "split",  "left",    "right",
        "exists", "constructor", "econstructor", "case", "elim",  "change",
        "contradiction", "exfalso", "now", "try", "repeat", "do",   "first",
        "solve",  "idtac",  "fail",    "unshelve", "eapply", "rename", "move",
        NULL,
    };
    for (int i = 0; kw[i]; i++) {
        if (strcmp(t, kw[i]) == 0) {
            return true;
        }
    }
    return false;
}

// ---- notation table --------------------------------------------------------

// Structural punctuation too common to safely treat as a notation operator.
static bool notation_op_too_common(const char *op, int len) {
    if (len != 1) {
        return false;
    }
    switch (op[0]) {
    case ';':
    case ',':
    case ':':
    case '|':
        return true;
    default:
        return false;
    }
}

// op is always a non-empty operator token (oplen >= 1). target can be NULL when a
// cross-file seed's target copy failed under OOM, so it is rejected here.
static void register_notation(RW *rw, const char *op, int oplen, const char *target) {
    if (!target || notation_op_too_common(op, oplen)) {
        return;
    }
    if (rw->notation_count >= rw->notation_cap) {
        int ncap = rw->notation_cap ? rw->notation_cap * 2 : 16;
        RocqNotation *n = (RocqNotation *)cbm_arena_alloc(rw->a, (size_t)ncap * sizeof(RocqNotation));
        if (!n) {
            return;
        }
        if (rw->notations) { // non-null implies notation_count > 0 (we have appended before)
            memcpy(n, rw->notations, (size_t)rw->notation_count * sizeof(RocqNotation));
        }
        rw->notations = n;
        rw->notation_cap = ncap;
    }
    char *op_copy = cbm_arena_strndup(rw->a, op, (size_t)oplen);
    if (!op_copy) {
        return; // OOM: never store a NULL operator (lookup_notation would deref it)
    }
    rw->notations[rw->notation_count].op = op_copy;
    rw->notations[rw->notation_count].target = target;
    rw->notation_count++;
}

// Most-recently-registered binding wins (later declarations shadow earlier ones).
static const char *lookup_notation(RW *rw, const char *op, int oplen) {
    for (int i = rw->notation_count - 1; i >= 0; i--) {
        const char *o = rw->notations[i].op;
        size_t n = strlen(o);
        if ((size_t)oplen == n && memcmp(o, op, n) == 0) {
            return rw->notations[i].target;
        }
    }
    return NULL;
}

// ---- graph emission --------------------------------------------------------

// Callers always pass a non-null owner (a definition's qualified_name) and a
// non-null callee (pre-checked at each call site), so no null guard is needed.
static void emit_call(RW *rw, const char *owner, const char *callee, int line) {
    CBMCall c = {0};
    c.callee_name = callee;
    c.enclosing_func_qn = owner;
    c.start_line = line;
    cbm_calls_push(&rw->result->calls, rw->a, c);
}

// Emit a definition; returns its index in result->defs (or -1). `name` is always
// non-null (callers pre-check node_text); `qn` can be NULL when build_qn's arena
// allocation failed under memory pressure, so that case is rejected here.
static int emit_def(RW *rw, const char *name, const char *qn, const char *label, int start_line,
                    int end_line) {
    if (!qn) {
        return -1;
    }
    CBMDefinition def = {0};
    def.name = name;
    def.qualified_name = qn;
    def.label = label;
    def.file_path = rw->rel_path;
    // start_line is line_of(...) >= 1 and end_line >= start_line at every call.
    def.start_line = (uint32_t)start_line;
    def.end_line = (uint32_t)end_line;
    def.complexity = 1;
    cbm_defs_push(&rw->result->defs, rw->a, def);
    return rw->result->defs.count - 1;
}

// Harvest reference tokens under `n` (skipping the subtree `skip`, e.g. the name
// node) as calls from `owner`: identifiers resolve by name; operators resolve
// through the notation table to the definition they expand to.
static void harvest_calls(RW *rw, TSNode n, TSNode skip, const char *owner) {
    // owner is non-null and n is a real node on every call (the recursion only
    // descends into ts_node_child results); only `skip` is optionally null.
    if (!ts_node_is_null(skip) && ts_node_eq(n, skip)) {
        return;
    }
    uint32_t cc = ts_node_child_count(n);
    if (cc == 0) {
        TSSymbol s = ts_node_symbol(n);
        if (s == RSYM_IDENT || s == RSYM_QUALID) {
            char *nm = node_text(rw, n);
            if (nm && !is_filtered_callee(nm)) {
                emit_call(rw, owner, nm, line_of(n));
            }
        } else if (s == RSYM_OPERATOR) {
            uint32_t b = ts_node_start_byte(n);
            uint32_t e = ts_node_end_byte(n);
            const char *tgt = lookup_notation(rw, rw->source + b, (int)(e - b));
            if (tgt) {
                emit_call(rw, owner, tgt, line_of(n));
            }
        }
        return;
    }
    // Walk children with a cursor (O(1)/step). ts_node_child(n, i) is O(i), so an
    // indexed loop would make a wide node (e.g. a long proof body) O(n^2).
    TSTreeCursor cur = ts_tree_cursor_new(n);
    for (bool ok = ts_tree_cursor_goto_first_child(&cur); ok;
         ok = ts_tree_cursor_goto_next_sibling(&cur)) {
        harvest_calls(rw, ts_tree_cursor_current_node(&cur), skip, owner);
    }
    ts_tree_cursor_delete(&cur);
}

// ---- per-node handlers -----------------------------------------------------

// A leaf-only child (constructor/field) under a type QN ⇒ a "Method" definition.
static void emit_type_member(RW *rw, TSNode member, const char *type_qn) {
    TSNode nm = field_name(member);
    // The parser always emits the name leaf as the node's name field, so nm is
    // non-null here; node_text still returns NULL if its arena copy fails (OOM).
    char *name = node_text(rw, nm);
    if (!name) {
        return;
    }
    int idx = emit_def(rw, name, build_qn(rw, type_qn, name), "Method", line_of(nm), line_of(nm));
    if (idx >= 0) {
        rw->result->defs.items[idx].parent_class = type_qn;
    }
}

// Definition / Theorem (label "Function"); body/proof references become calls.
static void handle_define(RW *rw, TSNode node, const char *scope) {
    TSNode nm = field_name(node);
    // The parser always emits the name leaf as the node's name field, so nm is
    // non-null here; node_text still returns NULL if its arena copy fails (OOM).
    char *name = node_text(rw, nm);
    if (!name) {
        return;
    }
    int idx = emit_def(rw, name, build_qn(rw, scope, name), "Function", line_of(nm),
                       (int)ts_node_end_point(node).row + 1);
    rw->last_def_idx = idx;
    if (idx >= 0) {
        harvest_calls(rw, node, nm, rw->result->defs.items[idx].qualified_name);
    }
}

// Inductive / Variant / Record / Class. `member_label` unused — members are
// always "Method"; `type_label` distinguishes Type vs Interface.
static void handle_type(RW *rw, TSNode node, const char *scope, const char *type_label) {
    // The type node has production 0 (variable arity: name + members), so its name
    // is the first child (the parser always emits it first), not a named field.
    TSNode nm = ts_node_named_child(node, 0); // child 0 → O(1)
    char *name = node_text(rw, nm);
    if (!name) {
        return;
    }
    const char *type_qn = build_qn(rw, scope, name);
    emit_def(rw, name, type_qn, type_label, line_of(nm), (int)ts_node_end_point(node).row + 1);
    rw->last_def_idx = -1;

    // Cursor walk (O(1)/step): indexed child access is O(i), quadratic for a type
    // with many constructors/fields.
    TSTreeCursor cur = ts_tree_cursor_new(node);
    for (bool ok = ts_tree_cursor_goto_first_child(&cur); ok;
         ok = ts_tree_cursor_goto_next_sibling(&cur)) {
        TSNode ch = ts_tree_cursor_current_node(&cur);
        TSSymbol s = ts_node_symbol(ch);
        if (s == RSYM_CONSTRUCTOR || s == RSYM_FIELD_DEF) {
            emit_type_member(rw, ch, type_qn);
        }
    }
    ts_tree_cursor_delete(&cur);
}

// Instance (label "Function"): the `class` head becomes a base class (an
// IMPLEMENTS edge downstream); the body is harvested for calls.
static void handle_instance(RW *rw, TSNode node, const char *scope) {
    TSNode nm = field_name(node);
    // The parser always emits the name leaf as the node's name field, so nm is
    // non-null here; node_text still returns NULL if its arena copy fails (OOM).
    char *name = node_text(rw, nm);
    if (!name) {
        return;
    }
    int idx = emit_def(rw, name, build_qn(rw, scope, name), "Function", line_of(nm),
                       (int)ts_node_end_point(node).row + 1);
    rw->last_def_idx = idx;
    if (idx < 0) {
        return;
    }
    const char *owner = rw->result->defs.items[idx].qualified_name;

    // child 1 (the `class` field) is the instantiated class head.
    TSNode cls = ts_node_child_by_field_name(node, "class", 5);
    if (!ts_node_is_null(cls)) {
        char *cname = node_text(rw, cls);
        if (cname && !is_filtered_callee(cname)) {
            const char **bc = (const char **)cbm_arena_alloc(rw->a, 2 * sizeof(char *));
            if (bc) {
                bc[0] = cname;
                bc[1] = NULL;
                rw->result->defs.items[idx].base_classes = bc;
            }
        }
    }
    harvest_calls(rw, node, nm, owner);
}

// Module / Module Type / Section (label "Module"): emit the node, then recurse
// into its body with the module's QN as the new scope so nesting is reflected in
// child QNs.
static void walk_children(RW *rw, TSNode parent, const char *scope);

static void handle_module(RW *rw, TSNode node, const char *scope) {
    // The module node has production 0 (variable arity: name + body), so its name
    // is the first child (the parser always emits it first), not a named field.
    TSNode nm = ts_node_named_child(node, 0); // child 0 → O(1)
    char *name = node_text(rw, nm);
    if (!name) {
        return;
    }
    const char *mod_qn = build_qn(rw, scope, name);
    emit_def(rw, name, mod_qn, "Module", line_of(nm), (int)ts_node_end_point(node).row + 1);
    rw->last_def_idx = -1;
    walk_children(rw, node, mod_qn);
}

// Parameter / Axiom / Variable / Hypothesis member ⇒ a "Variable" definition.
static void handle_assumption(RW *rw, TSNode node, const char *scope) {
    TSNode nm = field_name(node);
    // The parser always emits the name leaf as the node's name field, so nm is
    // non-null here; node_text still returns NULL if its arena copy fails (OOM).
    char *name = node_text(rw, nm);
    if (!name) {
        return;
    }
    emit_def(rw, name, build_qn(rw, scope, name), "Variable", line_of(nm), line_of(nm));
    rw->last_def_idx = -1;
}

// Notation / Infix (label "Variable") or Tactic Notation (label "Function") —
// distinguished by which field resolves (pattern ⇒ notation-form, name ⇒ Ltac).
static void handle_notation_or_tactic(RW *rw, TSNode node, const char *scope, bool is_tactic_sym) {
    TSNode pat = ts_node_child_by_field_name(node, "pattern", 7);
    if (ts_node_is_null(pat)) {
        // Ltac form: a plain named tactic (label "Function"). The node carries a
        // name leaf (nm non-null); node_text can still fail under OOM.
        TSNode nm = field_name(node);
        char *name = node_text(rw, nm);
        if (name) {
            emit_def(rw, name, build_qn(rw, scope, name), "Function", line_of(nm), line_of(nm));
        }
        rw->last_def_idx = -1;
        return;
    }

    // Notation form. The pattern leaf is the literal string token, which the
    // parser only emits with length >= 2 (the surrounding quotes), so the inner
    // span is always well-defined (possibly empty for "").
    uint32_t ps = ts_node_start_byte(pat);
    uint32_t pe = ts_node_end_byte(pat);
    const char *inner = rw->source + ps + 1;
    int inner_len = (int)(pe - ps) - 2;
    const char *label = is_tactic_sym ? "Function" : "Variable";
    char *pname = cbm_arena_strndup(rw->a, inner, (size_t)inner_len);
    int nidx = -1;
    if (pname && pname[0]) {
        nidx = emit_def(rw, pname, build_qn(rw, scope, pname), label, line_of(pat), line_of(pat));
    }
    rw->last_def_idx = -1;

    // The expansion head: first identifier under the term following `:=`.
    const char *target = NULL;
    int target_line = 0;
    // Cursor walk (O(1)/step) to find the term child, then its first identifier.
    TSTreeCursor cur = ts_tree_cursor_new(node);
    for (bool ok = ts_tree_cursor_goto_first_child(&cur); ok;
         ok = ts_tree_cursor_goto_next_sibling(&cur)) {
        TSNode ch = ts_tree_cursor_current_node(&cur);
        if (ts_node_symbol(ch) != RSYM_TERM) {
            continue;
        }
        TSTreeCursor tcur = ts_tree_cursor_new(ch);
        for (bool ok2 = ts_tree_cursor_goto_first_child(&tcur); ok2;
             ok2 = ts_tree_cursor_goto_next_sibling(&tcur)) {
            TSNode tok = ts_tree_cursor_current_node(&tcur);
            TSSymbol s = ts_node_symbol(tok);
            if (s == RSYM_IDENT || s == RSYM_QUALID) {
                char *t = node_text(rw, tok);
                if (t && !is_filtered_callee(t)) {
                    target = t;
                    target_line = line_of(tok);
                    break;
                }
            }
        }
        ts_tree_cursor_delete(&tcur);
        if (target) {
            break;
        }
    }
    ts_tree_cursor_delete(&cur);

    if (target) {
        if (nidx >= 0) {
            emit_call(rw, rw->result->defs.items[nidx].qualified_name, target, target_line);
        }
        // Register each literal operator in the pattern as a dynamic binding.
        RocqLexer il;
        rocq_lex_init(&il, inner, inner_len);
        for (;;) {
            RocqToken t = rocq_lex_next(&il);
            if (t.kind == ROCQ_TOK_EOF) {
                break;
            }
            if (t.kind == ROCQ_TOK_SYMBOL) {
                register_notation(rw, t.text, t.len, target);
            }
        }
    }
}

// Coercion: two ident leaves (A, B) ⇒ an A-implements/coerces-to-B edge.
static void handle_coercion(RW *rw, TSNode node) {
    const char *a = NULL;
    const char *b = NULL;
    // The parser emits exactly two ident/qualid leaves in a coercion node;
    // node_text only returns NULL on OOM. Cursor walk (O(1)/step).
    TSTreeCursor cur = ts_tree_cursor_new(node);
    for (bool ok = ts_tree_cursor_goto_first_child(&cur); ok;
         ok = ts_tree_cursor_goto_next_sibling(&cur)) {
        char *t = node_text(rw, ts_tree_cursor_current_node(&cur));
        if (!t) {
            continue;
        }
        if (!a) {
            a = t;
        } else {
            b = t; // the second identifier; stop scanning
            break;
        }
    }
    ts_tree_cursor_delete(&cur);
    if (a && b) {
        CBMImplTrait it = {0};
        it.struct_name = a;
        it.trait_name = b;
        cbm_impltrait_push(&rw->result->impl_traits, rw->a, it);
    }
    rw->last_def_idx = -1;
}

// Emit imports for a require/import node. With a `name` field the node is a
// `From <prefix> Require …` (prepend the prefix to each remaining leaf);
// otherwise each leaf is a complete logical module path.
static void emit_import_path(RW *rw, const char *path) {
    if (!path) { // NULL only on an arena allocation failure; the path is never empty
        return;
    }
    CBMImport imp = {0};
    imp.module_path = path;
    const char *leaf = path;
    const char *dot = strrchr(path, '.');
    if (dot) {
        leaf = dot + 1;
    }
    imp.local_name = cbm_arena_strdup(rw->a, leaf);
    cbm_imports_push(&rw->result->imports, rw->a, imp);
}

static void handle_require(RW *rw, TSNode node) {
    // A `From X Require …` node keeps a `name` field (the prefix X) and so has a
    // non-zero production with variable arity; a TSTreeCursor would read its
    // (absent) alias sequence, so iterate by index here. Require lists are short,
    // so the O(i) indexed access is fine.
    TSNode prefix = field_name(node);
    char *pfx = ts_node_is_null(prefix) ? NULL : node_text(rw, prefix);
    uint32_t cc = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < cc; i++) {
        TSNode ch = ts_node_named_child(node, i);
        if (!ts_node_is_null(prefix)) {
            if (ts_node_eq(ch, prefix)) {
                continue; // the From-prefix itself
            }
            char *mod = node_text(rw, ch);
            if (pfx && mod) {
                char buf[RW_QN_BUF];
                snprintf(buf, sizeof(buf), "%s.%s", pfx, mod);
                emit_import_path(rw, cbm_arena_strdup(rw->a, buf));
            }
        } else {
            emit_import_path(rw, node_text(rw, ch));
        }
    }
}

// Attribute a proof's harvested references to the most recent proof-bearing def,
// and stretch that def's end line to the proof's end (the Qed line).
static void handle_proof(RW *rw, TSNode node) {
    // last_def_idx is either -1 (no proof-bearing def yet) or a valid index.
    if (rw->last_def_idx < 0) {
        return;
    }
    const char *owner = rw->result->defs.items[rw->last_def_idx].qualified_name;
    TSNode none = {0};
    harvest_calls(rw, node, none, owner);
    int end = (int)ts_node_end_point(node).row + 1;
    if (end > (int)rw->result->defs.items[rw->last_def_idx].end_line) {
        rw->result->defs.items[rw->last_def_idx].end_line = (uint32_t)end;
    }
}

static void walk_command(RW *rw, TSNode node, const char *scope) {
    switch (ts_node_symbol(node)) {
    case RSYM_DEFINITION:
    case RSYM_THEOREM:
        handle_define(rw, node, scope);
        break;
    case RSYM_INDUCTIVE:
        handle_type(rw, node, scope, "Type");
        break;
    case RSYM_RECORD:
        handle_type(rw, node, scope, "Type");
        break;
    case RSYM_CLASS:
        handle_type(rw, node, scope, "Interface");
        break;
    case RSYM_INSTANCE:
        handle_instance(rw, node, scope);
        break;
    case RSYM_MODULE:
    case RSYM_MODULE_TYPE:
    case RSYM_SECTION:
        handle_module(rw, node, scope);
        break;
    case RSYM_ASSUMPTION:
        handle_assumption(rw, node, scope);
        break;
    case RSYM_NOTATION:
        handle_notation_or_tactic(rw, node, scope, false);
        break;
    case RSYM_TACTIC:
        handle_notation_or_tactic(rw, node, scope, true);
        break;
    case RSYM_COERCION:
        handle_coercion(rw, node);
        break;
    case RSYM_REQUIRE:
    case RSYM_IMPORT:
        handle_require(rw, node);
        break;
    case RSYM_PROOF:
        handle_proof(rw, node);
        break;
    default:
        break;
    }
}

static void walk_children(RW *rw, TSNode parent, const char *scope) {
    // Cursor walk (O(1)/step): a file/module with many top-level commands would be
    // O(n^2) under indexed child access (ts_node_named_child is O(i)).
    TSTreeCursor cur = ts_tree_cursor_new(parent);
    for (bool ok = ts_tree_cursor_goto_first_child(&cur); ok;
         ok = ts_tree_cursor_goto_next_sibling(&cur)) {
        walk_command(rw, ts_tree_cursor_current_node(&cur), scope);
    }
    ts_tree_cursor_delete(&cur);
}

// cbm_rocq_extract_file calls this only with a non-null tree (inside `if (tree)`)
// and a non-null module_qn (it returns earlier when module_qn is null).
void rocq_walk_tree(CBMArena *a, CBMFileResult *result, const void *tree, const char *source,
                    const char *module_qn, const char *rel_path) {
    RW rw = {0};
    rw.a = a;
    rw.result = result;
    rw.source = source;
    rw.module_qn = module_qn;
    rw.rel_path = cbm_arena_strdup(a, rel_path); // NULL-safe; rel_path is non-null in practice
    rw.last_def_idx = -1;

    // Seed notations imported from Require'd modules (resolved cross-file by the
    // pre-pass), so notation uses resolve across files; the file's own notations
    // layer on top in document order as the walk encounters them.
    const RocqSeedDB *seeddb = cbm_rocq_get_seeddb();
    if (seeddb && rw.rel_path) {
        const RocqNotationEntry *seed = NULL;
        int sn = rocq_seeddb_lookup(seeddb, rw.rel_path, &seed);
        for (int i = 0; i < sn; i++) {
            register_notation(&rw, seed[i].op, (int)strlen(seed[i].op),
                              cbm_arena_strdup(a, seed[i].target));
        }
    }

    TSNode root = ts_tree_root_node((const TSTree *)tree);
    walk_children(&rw, root, module_qn);
}
