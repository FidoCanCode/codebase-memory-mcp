// SPDX-License-Identifier: MIT
//
// rocq_parse.c — see rocq_parse.h. Original hand-written Vernacular parser.
#include "rocq/rocq_parse.h"
#include "rocq/rocq_lex.h"
#include "rocq/rocq_notation.h"

#include <stdio.h>
#include <string.h>

enum {
    RP_MAX_SCOPE = 128,
    RP_QN_BUF = 1024,
};

typedef enum {
    RP_SCOPE_MODULE,
    RP_SCOPE_MODTYPE,
    RP_SCOPE_SECTION,
} RpScopeKind;

typedef struct {
    RpScopeKind kind;
    const char *name; // arena-owned
} RpScope;

// A user-defined notation's dynamic binding: the literal operator token of the
// notation pattern (e.g. "+") mapped to the head identifier its RHS expands to
// (e.g. "Nat.add"). The table is the parser's extensibility point — populated
// when a Notation/Infix command is parsed, and consulted when resolving later
// uses, so a notation only affects text that follows its declaration.
typedef struct {
    const char *op;     // arena-owned literal operator token
    const char *target; // arena-owned head identifier of the expansion
} RocqNotation;

typedef struct {
    CBMArena *a;
    CBMFileResult *result;
    const char *module_qn;
    const char *rel_path; // arena-owned copy

    RocqLexer lx;
    RocqToken lookahead;
    bool have_lookahead;

    RpScope scopes[RP_MAX_SCOPE];
    int scope_count;

    // Proof state. last_def_idx tracks the most recent proof-bearing definition
    // so a following `Proof.` attaches its body's references to it.
    int last_def_idx;
    const char *proof_owner; // arena-owned QN, non-NULL while harvesting a proof

    RocqNotation *notations; // dynamic notation table (arena-grown), file-order
    int notation_count;
    int notation_cap;
} RP;

// ---- token stream with one-token lookahead --------------------------------

static RocqToken rp_next(RP *rp) {
    if (rp->have_lookahead) {
        rp->have_lookahead = false;
        return rp->lookahead;
    }
    return rocq_lex_next(&rp->lx);
}

static RocqToken rp_peek(RP *rp) {
    if (!rp->have_lookahead) {
        rp->lookahead = rocq_lex_next(&rp->lx);
        rp->have_lookahead = true;
    }
    return rp->lookahead;
}

static bool tok_eq(RocqToken t, const char *kw) {
    size_t n = strlen(kw);
    return t.kind == ROCQ_TOK_IDENT && (size_t)t.len == n && memcmp(t.text, kw, n) == 0;
}

static bool tok_is_assign(RocqToken t) {
    return t.kind == ROCQ_TOK_SYMBOL && t.len >= 2 && t.text[0] == ':' && t.text[1] == '=';
}

static bool tok_is_bar(RocqToken t) {
    return t.kind == ROCQ_TOK_SYMBOL && t.len == 1 && t.text[0] == '|';
}

static bool tok_is_semi(RocqToken t) {
    return t.kind == ROCQ_TOK_SYMBOL && t.len == 1 && t.text[0] == ';';
}

// ---- name / QN helpers -----------------------------------------------------

static void leaf_of(const char *text, int len, const char **lp, int *ll) {
    for (int k = len - 1; k >= 0; k--) {
        if (text[k] == '.') {
            *lp = text + k + 1;
            *ll = len - (k + 1);
            return;
        }
    }
    *lp = text;
    *ll = len;
}

// Build "<module_qn>[.<scope...>].<name>" in the arena.
static const char *build_qn(RP *rp, const char *name, int namelen) {
    char buf[RP_QN_BUF];
    int n = snprintf(buf, sizeof(buf), "%s", rp->module_qn);
    for (int i = 0; i < rp->scope_count && n < (int)sizeof(buf); i++) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, ".%s", rp->scopes[i].name);
    }
    if (n < (int)sizeof(buf)) {
        snprintf(buf + n, sizeof(buf) - (size_t)n, ".%.*s", namelen, name);
    }
    return cbm_arena_strdup(rp->a, buf);
}

static const char *build_child_qn(RP *rp, const char *parent_qn, const char *name, int namelen) {
    char buf[RP_QN_BUF];
    snprintf(buf, sizeof(buf), "%s.%.*s", parent_qn, namelen, name);
    return cbm_arena_strdup(rp->a, buf);
}

// Emit a top-level (scope-qualified) definition; returns its index in defs.
static int emit_def(RP *rp, const char *label, RocqToken name, int end_line) {
    CBMDefinition def = {0};
    def.name = cbm_arena_strndup(rp->a, name.text, (size_t)name.len);
    def.qualified_name = build_qn(rp, name.text, name.len);
    def.label = label;
    def.file_path = rp->rel_path;
    def.start_line = (uint32_t)name.line;
    def.end_line = (uint32_t)(end_line > 0 ? end_line : name.line);
    def.complexity = 1;
    cbm_defs_push(&rp->result->defs, rp->a, def);
    return rp->result->defs.count - 1;
}

// Emit a child (constructor/field) under a parent type QN.
static void emit_child(RP *rp, const char *parent_qn, RocqToken name) {
    CBMDefinition def = {0};
    def.name = cbm_arena_strndup(rp->a, name.text, (size_t)name.len);
    def.qualified_name = build_child_qn(rp, parent_qn, name.text, name.len);
    def.label = "Method";
    def.file_path = rp->rel_path;
    def.start_line = (uint32_t)name.line;
    def.end_line = (uint32_t)name.line;
    def.parent_class = parent_qn;
    def.complexity = 1;
    cbm_defs_push(&rp->result->defs, rp->a, def);
}

static void emit_call(RP *rp, const char *owner_qn, RocqToken callee) {
    CBMCall c = {0};
    c.callee_name = cbm_arena_strndup(rp->a, callee.text, (size_t)callee.len);
    c.enclosing_func_qn = owner_qn;
    c.start_line = callee.line;
    cbm_calls_push(&rp->result->calls, rp->a, c);
}

static void emit_import_logical(RP *rp, const char *text, int len) {
    if (len <= 0) {
        return;
    }
    CBMImport imp = {0};
    imp.module_path = cbm_arena_strndup(rp->a, text, (size_t)len);
    const char *lp;
    int ll;
    leaf_of(text, len, &lp, &ll);
    imp.local_name = cbm_arena_strndup(rp->a, lp, (size_t)ll);
    cbm_imports_push(&rp->result->imports, rp->a, imp);
}

// ---- callee filter ---------------------------------------------------------

// Gallina / Ltac keywords that are never useful as proof-dependency targets.
// Filtering them only saves resolution work — unknown names that survive are
// simply discarded by the registry resolver, so the set need not be exhaustive.
static bool is_filtered_callee(const char *t, int len) {
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
        size_t n = strlen(kw[i]);
        if ((size_t)len == n && memcmp(t, kw[i], n) == 0) {
            return true;
        }
    }
    return false;
}

// ---- dynamic notation table -----------------------------------------------

// Emit a call to a resolved target name (arena-owned), not a source token.
static void emit_call_name(RP *rp, const char *owner_qn, const char *name, int line) {
    if (!owner_qn || !name) {
        return;
    }
    CBMCall c = {0};
    c.callee_name = name;
    c.enclosing_func_qn = owner_qn;
    c.start_line = line;
    cbm_calls_push(&rp->result->calls, rp->a, c);
}

// Structural punctuation that is too common to safely treat as a notation
// operator (a notation reusing ';'/','/etc. would otherwise match everywhere).
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

static void register_notation(RP *rp, const char *op, int oplen, const char *target) {
    if (oplen <= 0 || !target || notation_op_too_common(op, oplen)) {
        return;
    }
    if (rp->notation_count >= rp->notation_cap) {
        int ncap = rp->notation_cap ? rp->notation_cap * 2 : 16;
        RocqNotation *n = (RocqNotation *)cbm_arena_alloc(rp->a, (size_t)ncap * sizeof(RocqNotation));
        if (!n) {
            return;
        }
        if (rp->notations && rp->notation_count > 0) {
            memcpy(n, rp->notations, (size_t)rp->notation_count * sizeof(RocqNotation));
        }
        rp->notations = n;
        rp->notation_cap = ncap;
    }
    rp->notations[rp->notation_count].op = cbm_arena_strndup(rp->a, op, (size_t)oplen);
    rp->notations[rp->notation_count].target = target;
    rp->notation_count++;
}

// Most-recently-registered notation for an operator token wins (later
// declarations shadow earlier ones), matching Rocq's file-order semantics.
static const char *lookup_notation(RP *rp, const char *op, int oplen) {
    for (int i = rp->notation_count - 1; i >= 0; i--) {
        const char *o = rp->notations[i].op;
        size_t n = strlen(o);
        if ((size_t)oplen == n && memcmp(o, op, n) == 0) {
            return rp->notations[i].target;
        }
    }
    return NULL;
}

// Harvest one body/proof token as a dependency of owner_qn: an identifier
// resolves by name (the registry resolves it later); a notation operator
// resolves through the dynamic table to the definition it expands to.
static void harvest_token(RP *rp, const char *owner_qn, RocqToken t) {
    if (!owner_qn) {
        return;
    }
    if (t.kind == ROCQ_TOK_IDENT) {
        if (!is_filtered_callee(t.text, t.len)) {
            emit_call(rp, owner_qn, t);
        }
    } else if (t.kind == ROCQ_TOK_SYMBOL) {
        const char *tgt = lookup_notation(rp, t.text, t.len);
        if (tgt) {
            emit_call_name(rp, owner_qn, tgt, t.line);
        }
    }
}

// ---- scope management ------------------------------------------------------

static void scope_push(RP *rp, RpScopeKind kind, RocqToken name) {
    if (rp->scope_count >= RP_MAX_SCOPE) {
        return;
    }
    rp->scopes[rp->scope_count].kind = kind;
    rp->scopes[rp->scope_count].name = cbm_arena_strndup(rp->a, name.text, (size_t)name.len);
    rp->scope_count++;
}

static void scope_pop(RP *rp) {
    if (rp->scope_count > 0) {
        rp->scope_count--;
    }
}

// ---- per-command consumption helpers --------------------------------------

static void skip_to_dot(RP *rp) {
    for (;;) {
        RocqToken t = rp_next(rp);
        if (t.kind == ROCQ_TOK_DOT || t.kind == ROCQ_TOK_EOF) {
            return;
        }
    }
}

// Harvest every non-filtered identifier in the rest of the command as a call
// from owner_qn. Returns the line of the terminating dot (or last token).
static int harvest_to_dot(RP *rp, const char *owner_qn) {
    int last_line = 0;
    for (;;) {
        RocqToken t = rp_next(rp);
        if (t.kind == ROCQ_TOK_DOT || t.kind == ROCQ_TOK_EOF) {
            return t.line ? t.line : last_line;
        }
        last_line = t.line;
        harvest_token(rp, owner_qn, t);
    }
}

// ---- command handlers ------------------------------------------------------

// Definition / Theorem / Lemma / Fixpoint / Instance / ...  (label "Function").
// Reads the name, emits the def, then harvests body references as calls. Honors
// `with` for mutually-recursive groups.
static void h_define(RP *rp, const char *label) {
    RocqToken t = rp_peek(rp);
    while (t.kind != ROCQ_TOK_IDENT && t.kind != ROCQ_TOK_DOT && t.kind != ROCQ_TOK_EOF) {
        rp_next(rp);
        t = rp_peek(rp);
    }
    if (t.kind != ROCQ_TOK_IDENT) {
        skip_to_dot(rp);
        rp->last_def_idx = -1;
        return;
    }
    RocqToken name = rp_next(rp);
    int idx = emit_def(rp, label, name, 0);
    rp->last_def_idx = idx;
    const char *owner = rp->result->defs.items[idx].qualified_name;

    for (;;) {
        RocqToken u = rp_next(rp);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            return;
        }
        if (u.kind == ROCQ_TOK_IDENT && tok_eq(u, "with")) {
            RocqToken n2 = rp_peek(rp);
            if (n2.kind == ROCQ_TOK_IDENT) {
                rp_next(rp);
                int j = emit_def(rp, label, n2, 0);
                owner = rp->result->defs.items[j].qualified_name;
                rp->last_def_idx = j;
            }
            continue;
        }
        harvest_token(rp, owner, u);
    }
}

typedef enum { RP_TYPE_INDUCTIVE, RP_TYPE_RECORD } RpTypeMode;

// Inductive / Variant / Record / Class (label "Type" with "Method" children).
static void h_type(RP *rp, RpTypeMode mode, const char *type_label) {
    RocqToken t = rp_peek(rp);
    if (t.kind != ROCQ_TOK_IDENT) {
        skip_to_dot(rp);
        return;
    }
    RocqToken name = rp_next(rp);
    int idx = emit_def(rp, type_label, name, 0);
    rp->last_def_idx = idx;
    const char *parent = rp->result->defs.items[idx].qualified_name;

    bool child_next = false; // next ident is a constructor/field name
    int brace_depth = 0;
    for (;;) {
        RocqToken u = rp_next(rp);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            return;
        }
        if (mode == RP_TYPE_RECORD) {
            if (u.kind == ROCQ_TOK_LBRACE) {
                brace_depth++;
                child_next = (brace_depth == 1);
                continue;
            }
            if (u.kind == ROCQ_TOK_RBRACE) {
                if (brace_depth > 0) {
                    brace_depth--;
                }
                child_next = false;
                continue;
            }
            if (brace_depth == 1 && tok_is_semi(u)) {
                child_next = true;
                continue;
            }
            if (child_next && u.kind == ROCQ_TOK_IDENT) {
                emit_child(rp, parent, u);
                child_next = false;
            }
            continue;
        }
        // RP_TYPE_INDUCTIVE
        if (tok_is_assign(u) || tok_is_bar(u)) {
            child_next = true;
            continue;
        }
        if (u.kind == ROCQ_TOK_IDENT && tok_eq(u, "with")) {
            RocqToken n2 = rp_peek(rp);
            if (n2.kind == ROCQ_TOK_IDENT) {
                rp_next(rp);
                int j = emit_def(rp, type_label, n2, 0);
                parent = rp->result->defs.items[j].qualified_name;
                rp->last_def_idx = j;
            }
            child_next = false;
            continue;
        }
        if (child_next && u.kind == ROCQ_TOK_IDENT) {
            emit_child(rp, parent, u);
            child_next = false;
        }
    }
}

// Module / Module Type / Section (label "Module"). Pushes a scope for body
// modules; `:=` aliases declare no body and do not push.
static void h_module(RP *rp, bool is_section) {
    RpScopeKind kind = is_section ? RP_SCOPE_SECTION : RP_SCOPE_MODULE;
    RocqToken t = rp_peek(rp);
    if (!is_section && tok_eq(t, "Type")) {
        rp_next(rp);
        kind = RP_SCOPE_MODTYPE;
        t = rp_peek(rp);
    }
    if (t.kind != ROCQ_TOK_IDENT) {
        skip_to_dot(rp);
        return;
    }
    RocqToken name = rp_next(rp);
    emit_def(rp, "Module", name, 0);
    rp->last_def_idx = -1;

    bool has_assign = false;
    for (;;) {
        RocqToken u = rp_next(rp);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            break;
        }
        if (tok_is_assign(u)) {
            has_assign = true;
        }
    }
    if (is_section || !has_assign) {
        scope_push(rp, kind, name);
    }
}

// Parameter / Axiom / Variable / Hypothesis ... (label "Variable").
// Emits one Variable per name appearing before the first ':'.
static void h_assume(RP *rp) {
    for (;;) {
        RocqToken u = rp_next(rp);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            return;
        }
        if (u.kind == ROCQ_TOK_SYMBOL || u.kind == ROCQ_TOK_LPAREN || u.kind == ROCQ_TOK_LBRACE) {
            // reached the type annotation / binder section — stop naming.
            skip_to_dot(rp);
            return;
        }
        if (u.kind == ROCQ_TOK_IDENT) {
            emit_def(rp, "Variable", u, 0);
        }
    }
}

// Ltac / Ltac2 tactic definition (label "Function"); body left opaque.
static void h_ltac(RP *rp) {
    RocqToken t = rp_peek(rp);
    if (t.kind == ROCQ_TOK_IDENT) {
        rp_next(rp);
        emit_def(rp, "Function", t, 0);
    }
    rp->last_def_idx = -1;
    skip_to_dot(rp);
}

// Notation / Infix (label "Variable", low priority). Names the node after the
// notation string literal; bodies are opaque.
static void h_notation(RP *rp) {
    // Pattern: Notation "<pattern>" := (<head> ...).  Capture the pattern string
    // and the head identifier of the expansion, then register each literal
    // operator in the pattern as a dynamic binding to that head.
    RocqToken str = {0};
    bool have_str = false;
    bool seen_assign = false;
    const char *target = NULL;
    int target_line = 0;
    for (;;) {
        RocqToken u = rp_next(rp);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            break;
        }
        if (!have_str && u.kind == ROCQ_TOK_STRING && u.len >= 2) {
            str = u;
            have_str = true;
            continue;
        }
        if (tok_is_assign(u)) {
            seen_assign = true;
            continue;
        }
        if (seen_assign && !target && u.kind == ROCQ_TOK_IDENT &&
            !is_filtered_callee(u.text, u.len)) {
            target = cbm_arena_strndup(rp->a, u.text, (size_t)u.len);
            target_line = u.line;
        }
    }

    int nidx = -1;
    if (have_str) {
        RocqToken inner = str;
        inner.text = str.text + 1; // strip the surrounding quotes
        inner.len = str.len - 2;
        if (inner.len > 0) {
            nidx = emit_def(rp, "Variable", inner, 0);
        }
    }
    rp->last_def_idx = -1;

    if (have_str && target) {
        // The notation node depends on the definition it expands to.
        if (nidx >= 0) {
            emit_call_name(rp, rp->result->defs.items[nidx].qualified_name, target, target_line);
        }
        // Register each literal operator token in the pattern. Re-lex the inner
        // pattern: identifiers are notation parameters; SYMBOL runs are the
        // literal operators that denote the expansion at use sites.
        RocqLexer il;
        rocq_lex_init(&il, str.text + 1, str.len - 2);
        for (;;) {
            RocqToken t = rocq_lex_next(&il);
            if (t.kind == ROCQ_TOK_EOF) {
                break;
            }
            if (t.kind == ROCQ_TOK_SYMBOL) {
                register_notation(rp, t.text, t.len, target);
            }
        }
    }
}

// Require [Import|Export] A.B C ...  — emit one import per logical module name.
static void h_require(RP *rp) {
    for (;;) {
        RocqToken u = rp_next(rp);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            return;
        }
        if (u.kind != ROCQ_TOK_IDENT) {
            continue;
        }
        if (tok_eq(u, "Import") || tok_eq(u, "Export")) {
            continue;
        }
        emit_import_logical(rp, u.text, u.len);
    }
}

// From X Require [Import|Export] Y Z  — emit imports for X.Y, X.Z.
static void h_from(RP *rp) {
    RocqToken pfx = rp_next(rp);
    if (pfx.kind != ROCQ_TOK_IDENT) {
        if (pfx.kind != ROCQ_TOK_DOT && pfx.kind != ROCQ_TOK_EOF) {
            skip_to_dot(rp);
        }
        return;
    }
    // advance to the Require keyword
    RocqToken t;
    for (;;) {
        t = rp_next(rp);
        if (t.kind == ROCQ_TOK_DOT || t.kind == ROCQ_TOK_EOF) {
            return;
        }
        if (tok_eq(t, "Require")) {
            break;
        }
    }
    for (;;) {
        RocqToken u = rp_next(rp);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            return;
        }
        if (u.kind != ROCQ_TOK_IDENT) {
            continue;
        }
        if (tok_eq(u, "Import") || tok_eq(u, "Export")) {
            continue;
        }
        char buf[RP_QN_BUF];
        int n = snprintf(buf, sizeof(buf), "%.*s.%.*s", pfx.len, pfx.text, u.len, u.text);
        if (n > 0) {
            emit_import_logical(rp, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
        }
    }
}

// Import / Export <module> (standalone — opens an already-required module).
static void h_open_import(RP *rp) {
    for (;;) {
        RocqToken u = rp_next(rp);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            return;
        }
        if (u.kind == ROCQ_TOK_IDENT) {
            emit_import_logical(rp, u.text, u.len);
        }
    }
}

static void h_proof(RP *rp) {
    if (rp->last_def_idx >= 0) {
        rp->proof_owner = rp->result->defs.items[rp->last_def_idx].qualified_name;
    } else {
        rp->proof_owner = NULL;
    }
    skip_to_dot(rp);
}

static void h_end_proof(RP *rp) {
    int end_line = harvest_to_dot(rp, NULL); // consume; the keyword line ends the proof
    if (rp->last_def_idx >= 0 && end_line > 0) {
        rp->result->defs.items[rp->last_def_idx].end_line = (uint32_t)end_line;
    }
    rp->proof_owner = NULL;
    rp->last_def_idx = -1;
}

// Instance (label "Function"): a typeclass instance. Captures the class being
// instantiated — the head of the type after the top-level ':' — as a base
// class, which pass_semantic turns into an IMPLEMENTS edge to the Class
// (Interface). The body is harvested for calls like any definition.
static void h_instance(RP *rp) {
    RocqToken t = rp_peek(rp);
    while (t.kind != ROCQ_TOK_IDENT && t.kind != ROCQ_TOK_DOT && t.kind != ROCQ_TOK_EOF) {
        rp_next(rp);
        t = rp_peek(rp);
    }
    if (t.kind != ROCQ_TOK_IDENT) {
        skip_to_dot(rp);
        rp->last_def_idx = -1;
        return;
    }
    RocqToken name = rp_next(rp);
    int idx = emit_def(rp, "Function", name, 0);
    rp->last_def_idx = idx;
    const char *owner = rp->result->defs.items[idx].qualified_name;

    int depth = 0;
    bool want_class = false;
    const char *class_name = NULL;
    for (;;) {
        RocqToken u = rp_next(rp);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            break;
        }
        if (u.kind == ROCQ_TOK_LPAREN || u.kind == ROCQ_TOK_LBRACE || u.kind == ROCQ_TOK_LBRACK) {
            depth++;
            continue;
        }
        if (u.kind == ROCQ_TOK_RPAREN || u.kind == ROCQ_TOK_RBRACE || u.kind == ROCQ_TOK_RBRACK) {
            if (depth > 0) {
                depth--;
            }
            continue;
        }
        // The top-level ':' (not part of binders) introduces the class type.
        if (depth == 0 && !want_class && !class_name && u.kind == ROCQ_TOK_SYMBOL && u.len == 1 &&
            u.text[0] == ':') {
            want_class = true;
            continue;
        }
        if (want_class && !class_name && depth == 0 && u.kind == ROCQ_TOK_IDENT &&
            !is_filtered_callee(u.text, u.len)) {
            class_name = cbm_arena_strndup(rp->a, u.text, (size_t)u.len);
            want_class = false;
        }
        harvest_token(rp, owner, u);
    }
    if (class_name) {
        const char **bc = (const char **)cbm_arena_alloc(rp->a, 2 * sizeof(char *));
        if (bc) {
            bc[0] = class_name;
            bc[1] = NULL;
            rp->result->defs.items[idx].base_classes = bc;
        }
    }
}

// Returns true if the keyword token was dispatched as a known command.
static bool dispatch_keyword(RP *rp, RocqToken kw) {
    if (tok_eq(kw, "Definition") || tok_eq(kw, "Let") || tok_eq(kw, "Example") ||
        tok_eq(kw, "Fixpoint") || tok_eq(kw, "CoFixpoint") || tok_eq(kw, "Function") ||
        tok_eq(kw, "Theorem") || tok_eq(kw, "Lemma") || tok_eq(kw, "Corollary") ||
        tok_eq(kw, "Proposition") || tok_eq(kw, "Remark") || tok_eq(kw, "Fact") ||
        tok_eq(kw, "Property")) {
        h_define(rp, "Function");
        return true;
    }
    if (tok_eq(kw, "Instance")) {
        h_instance(rp);
        return true;
    }
    if (tok_eq(kw, "Inductive") || tok_eq(kw, "CoInductive") || tok_eq(kw, "Variant")) {
        h_type(rp, RP_TYPE_INDUCTIVE, "Type");
        return true;
    }
    if (tok_eq(kw, "Record") || tok_eq(kw, "Structure")) {
        h_type(rp, RP_TYPE_RECORD, "Type");
        return true;
    }
    if (tok_eq(kw, "Class")) {
        // A typeclass models an interface: instances IMPLEMENT it.
        h_type(rp, RP_TYPE_RECORD, "Interface");
        return true;
    }
    if (tok_eq(kw, "Module")) {
        h_module(rp, false);
        return true;
    }
    if (tok_eq(kw, "Section")) {
        h_module(rp, true);
        return true;
    }
    if (tok_eq(kw, "End")) {
        scope_pop(rp);
        skip_to_dot(rp);
        return true;
    }
    if (tok_eq(kw, "Parameter") || tok_eq(kw, "Parameters") || tok_eq(kw, "Axiom") ||
        tok_eq(kw, "Axioms") || tok_eq(kw, "Conjecture") || tok_eq(kw, "Variable") ||
        tok_eq(kw, "Variables") || tok_eq(kw, "Hypothesis") || tok_eq(kw, "Hypotheses")) {
        h_assume(rp);
        return true;
    }
    if (tok_eq(kw, "Ltac") || tok_eq(kw, "Ltac2")) {
        h_ltac(rp);
        return true;
    }
    if (tok_eq(kw, "Notation") || tok_eq(kw, "Infix")) {
        h_notation(rp);
        return true;
    }
    if (tok_eq(kw, "Require")) {
        h_require(rp);
        return true;
    }
    if (tok_eq(kw, "From")) {
        h_from(rp);
        return true;
    }
    if (tok_eq(kw, "Import") || tok_eq(kw, "Export")) {
        h_open_import(rp);
        return true;
    }
    if (tok_eq(kw, "Proof")) {
        h_proof(rp);
        return true;
    }
    if (tok_eq(kw, "Qed") || tok_eq(kw, "Defined") || tok_eq(kw, "Admitted") ||
        tok_eq(kw, "Abort") || tok_eq(kw, "Save")) {
        h_end_proof(rp);
        return true;
    }
    if (tok_eq(kw, "Goal")) {
        rp->last_def_idx = -1;
        skip_to_dot(rp);
        return true;
    }
    return false;
}

// Consume command prefixes (attributes and modifier keywords) so the real
// command keyword can be dispatched. Returns the keyword token, or an EOF
// token at end of input.
static RocqToken read_command_keyword(RP *rp) {
    for (;;) {
        RocqToken t = rp_peek(rp);
        if (t.kind == ROCQ_TOK_EOF) {
            return rp_next(rp);
        }
        if (t.kind == ROCQ_TOK_DOT) {
            rp_next(rp); // empty command
            continue;
        }
        // Attribute: #[ ... ]
        if (t.kind == ROCQ_TOK_SYMBOL && t.len >= 1 && t.text[0] == '#') {
            rp_next(rp);
            RocqToken b = rp_peek(rp);
            if (b.kind == ROCQ_TOK_LBRACK) {
                int depth = 0;
                for (;;) {
                    RocqToken u = rp_next(rp);
                    if (u.kind == ROCQ_TOK_EOF) {
                        return u;
                    }
                    if (u.kind == ROCQ_TOK_LBRACK) {
                        depth++;
                    } else if (u.kind == ROCQ_TOK_RBRACK) {
                        if (--depth <= 0) {
                            break;
                        }
                    }
                }
            }
            continue;
        }
        // Modifier prefixes that precede a real command keyword.
        if (tok_eq(t, "Local") || tok_eq(t, "Global") || tok_eq(t, "Polymorphic") ||
            tok_eq(t, "Monomorphic") || tok_eq(t, "Program") || tok_eq(t, "Cumulative") ||
            tok_eq(t, "NonCumulative") || tok_eq(t, "Private") || tok_eq(t, "Reserved") ||
            tok_eq(t, "Existing") || tok_eq(t, "Canonical")) {
            // "Existing Instance" / "Canonical Structure" have no nameable head
            // we model; just drop the modifier and re-dispatch the next token.
            rp_next(rp);
            continue;
        }
        return rp_next(rp);
    }
}

void rocq_parse_file(CBMArena *a, CBMFileResult *result, const char *source, int source_len,
                     const char *module_qn, const char *rel_path) {
    RP rp = {0};
    rp.a = a;
    rp.result = result;
    rp.module_qn = module_qn;
    rp.rel_path = rel_path ? cbm_arena_strdup(a, rel_path) : NULL;
    rp.last_def_idx = -1;
    rocq_lex_init(&rp.lx, source, source_len);

    // Seed notations imported from Require'd modules (resolved cross-file by the
    // pre-pass), so notation uses resolve across files. The file's own notations
    // layer on top in file order as they are parsed.
    const RocqSeedDB *seeddb = cbm_rocq_get_seeddb();
    if (seeddb && rp.rel_path) {
        const RocqNotationEntry *seed = NULL;
        int sn = rocq_seeddb_lookup(seeddb, rp.rel_path, &seed);
        for (int i = 0; i < sn; i++) {
            register_notation(&rp, seed[i].op, (int)strlen(seed[i].op),
                              cbm_arena_strdup(a, seed[i].target));
        }
    }

    for (;;) {
        RocqToken kw = read_command_keyword(&rp);
        if (kw.kind == ROCQ_TOK_EOF) {
            break;
        }
        if (kw.kind == ROCQ_TOK_IDENT && dispatch_keyword(&rp, kw)) {
            continue;
        }
        // Not a recognized command. Inside a proof, the line is a tactic step:
        // harvest its references. The keyword token itself is a candidate too.
        if (rp.proof_owner) {
            harvest_token(&rp, rp.proof_owner, kw);
            harvest_to_dot(&rp, rp.proof_owner);
        } else {
            skip_to_dot(&rp);
        }
    }
}
