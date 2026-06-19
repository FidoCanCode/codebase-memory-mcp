// SPDX-License-Identifier: MIT
//
// rocq_parse.c — see rocq_parse.h. Original hand-written Vernacular parser that
// builds a tree-sitter TSTree. Purely syntactic: it opens/closes nodes and emits
// leaves; it computes no qualified names and resolves no references.
#include "rocq/rocq_parse.h"
#include "rocq/rocq_cst.h"
#include "rocq/rocq_lex.h"
#include "rocq/rocq_tree.h"

#include <stdbool.h>
#include <string.h>

typedef struct {
    RocqLexer lx;
    RocqToken lookahead;
    bool have_lookahead;
    RocqTreeBuilder *tb;
    bool proof_open; // a `Proof. … Qed.` block is currently capturing tactic tokens
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

static bool tok_is_colon(RocqToken t) {
    return t.kind == ROCQ_TOK_SYMBOL && t.len == 1 && t.text[0] == ':';
}

// ---- tree-builder shims ----------------------------------------------------

// Emit a leaf for token `t` carrying symbol `sym`.
static void tb_leaf_tok(RP *rp, RocqSymbol sym, RocqToken t) {
    rocq_tb_leaf(rp->tb, sym, (uint32_t)t.start_byte, (uint32_t)(t.start_byte + t.len));
}

// The leaf symbol for an arbitrary token, or RSYM_END to mean "structural
// punctuation — not a leaf" (parens/braces/dots/EOF are tree padding).
static RocqSymbol leaf_sym_of(RocqToken t) {
    switch (t.kind) {
    case ROCQ_TOK_IDENT:
        return memchr(t.text, '.', (size_t)t.len) ? RSYM_QUALID : RSYM_IDENT;
    case ROCQ_TOK_NUMBER:
        return RSYM_NUMBER;
    case ROCQ_TOK_STRING:
        return RSYM_STRING;
    case ROCQ_TOK_SYMBOL:
        return RSYM_OPERATOR;
    default:
        return RSYM_END;
    }
}

// An identifier leaf symbol (qualid when dotted).
static RocqSymbol ident_sym_of(RocqToken t) {
    return memchr(t.text, '.', (size_t)t.len) ? RSYM_QUALID : RSYM_IDENT;
}

// Push a reference token into a lazily-opened `term` node (so empty bodies emit
// no node). Punctuation is dropped (recorded only as padding).
static void body_token(RP *rp, RocqToken t, bool *term_open) {
    RocqSymbol s = leaf_sym_of(t);
    if (s == RSYM_END) {
        return;
    }
    if (!*term_open) {
        rocq_tb_open(rp->tb, RSYM_TERM, RPROD_NONE);
        *term_open = true;
    }
    tb_leaf_tok(rp, s, t);
}

static void close_term(RP *rp, bool *term_open) {
    if (*term_open) {
        rocq_tb_close(rp->tb);
        *term_open = false;
    }
}

// Emit a leaf-only child node (constructor/field/assumption): [ident(name)].
static void emit_named_leaf_node(RP *rp, RocqSymbol node_sym, RocqToken name) {
    rocq_tb_open(rp->tb, node_sym, RPROD_NAME);
    tb_leaf_tok(rp, RSYM_IDENT, name);
    rocq_tb_close(rp->tb);
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

// ---- command handlers ------------------------------------------------------

// Definition / Theorem / Lemma / Fixpoint / Instance-less defs. Emits a command
// node named after the head identifier, then a `term` holding the body's
// reference tokens. `with` starts a sibling node in a mutually-recursive group.
static void h_define(RP *rp, RocqSymbol cmd_sym) {
    RocqToken t = rp_peek(rp);
    while (t.kind != ROCQ_TOK_IDENT && t.kind != ROCQ_TOK_DOT && t.kind != ROCQ_TOK_EOF) {
        rp_next(rp);
        t = rp_peek(rp);
    }
    if (t.kind != ROCQ_TOK_IDENT) {
        skip_to_dot(rp);
        return;
    }
    rocq_tb_open(rp->tb, cmd_sym, RPROD_NAME);
    RocqToken name = rp_next(rp);
    tb_leaf_tok(rp, RSYM_IDENT, name);

    bool term_open = false;
    for (;;) {
        RocqToken u = rp_next(rp);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            close_term(rp, &term_open);
            rocq_tb_close(rp->tb);
            return;
        }
        if (u.kind == ROCQ_TOK_IDENT && tok_eq(u, "with")) {
            RocqToken n2 = rp_peek(rp);
            if (n2.kind == ROCQ_TOK_IDENT) {
                close_term(rp, &term_open);
                rocq_tb_close(rp->tb); // close current clause
                rp_next(rp);
                rocq_tb_open(rp->tb, cmd_sym, RPROD_NAME);
                tb_leaf_tok(rp, RSYM_IDENT, n2);
            }
            continue;
        }
        body_token(rp, u, &term_open);
    }
}

typedef enum { RP_TYPE_INDUCTIVE, RP_TYPE_RECORD } RpTypeMode;

// Inductive / Variant / Record / Class: a type node whose children include the
// name (field) and one constructor/field node per member.
static void h_type(RP *rp, RpTypeMode mode, RocqSymbol type_sym) {
    RocqToken t = rp_peek(rp);
    if (t.kind != ROCQ_TOK_IDENT) {
        skip_to_dot(rp);
        return;
    }
    rocq_tb_open(rp->tb, type_sym, RPROD_NONE); // variable arity (name + members) → no field
    RocqToken name = rp_next(rp);
    tb_leaf_tok(rp, RSYM_IDENT, name);

    bool child_next = false; // next ident is a constructor/field name
    int brace_depth = 0;
    for (;;) {
        RocqToken u = rp_next(rp);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            rocq_tb_close(rp->tb);
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
                emit_named_leaf_node(rp, RSYM_FIELD_DEF, u);
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
                rocq_tb_close(rp->tb); // close current type
                rp_next(rp);
                rocq_tb_open(rp->tb, type_sym, RPROD_NONE); // variable arity (name + members) → no field
                tb_leaf_tok(rp, RSYM_IDENT, n2);
            }
            child_next = false;
            continue;
        }
        if (child_next && u.kind == ROCQ_TOK_IDENT) {
            emit_named_leaf_node(rp, RSYM_CONSTRUCTOR, u);
            child_next = false;
        }
    }
}

// Module / Module Type / Section. The container node is left *open* so the body
// commands nest beneath it; a matching `End` closes it. A `:=` alias module has
// no body and is closed immediately.
static void h_module(RP *rp, RocqSymbol container_sym, bool is_section) {
    RocqSymbol sym = container_sym;
    RocqToken t = rp_peek(rp);
    if (!is_section && tok_eq(t, "Type")) {
        rp_next(rp);
        sym = RSYM_MODULE_TYPE;
        t = rp_peek(rp);
    }
    if (t.kind != ROCQ_TOK_IDENT) {
        skip_to_dot(rp);
        return;
    }
    rocq_tb_open(rp->tb, sym, RPROD_NONE); // variable arity (name + body) → no field
    RocqToken name = rp_next(rp);
    tb_leaf_tok(rp, RSYM_IDENT, name);

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
    if (!is_section && has_assign) {
        rocq_tb_close(rp->tb); // alias declares no body
    }
    // Otherwise the node stays open; `End` (or rocq_tb_finish at EOF) closes it.
}

// Parameter / Axiom / Variable / Hypothesis …: one assumption node per name
// appearing before the first ':'/binder.
static void h_assume(RP *rp) {
    for (;;) {
        RocqToken u = rp_next(rp);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            return;
        }
        if (u.kind == ROCQ_TOK_SYMBOL || u.kind == ROCQ_TOK_LPAREN || u.kind == ROCQ_TOK_LBRACE) {
            skip_to_dot(rp); // reached the type annotation / binder section
            return;
        }
        if (u.kind == ROCQ_TOK_IDENT) {
            emit_named_leaf_node(rp, RSYM_ASSUMPTION, u);
        }
    }
}

// Ltac / Ltac2 tactic definition; body left opaque.
static void h_ltac(RP *rp) {
    RocqToken t = rp_peek(rp);
    if (t.kind == ROCQ_TOK_IDENT) {
        rp_next(rp);
        emit_named_leaf_node(rp, RSYM_TACTIC, t);
    }
    skip_to_dot(rp);
}

// Notation / Infix / Tactic Notation. The node's `pattern` field is the literal
// pattern string; the `term` after `:=` holds the expansion tokens (the walk
// reads the head identifier and registers the pattern's operators).
static void h_notation(RP *rp, RocqSymbol node_sym) {
    bool opened = false;
    bool seen_assign = false;
    bool term_open = false;
    for (;;) {
        RocqToken u = rp_next(rp);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            break;
        }
        if (!opened && u.kind == ROCQ_TOK_STRING && u.len >= 2) {
            rocq_tb_open(rp->tb, node_sym, RPROD_NOTATION);
            opened = true;
            tb_leaf_tok(rp, RSYM_STRING, u); // pattern @ child 0
            continue;
        }
        if (tok_is_assign(u)) {
            seen_assign = true;
            continue;
        }
        if (opened && seen_assign) {
            body_token(rp, u, &term_open);
        }
    }
    if (opened) {
        close_term(rp, &term_open);
        rocq_tb_close(rp->tb);
    }
}

// Tactic Notation "<pat>" := (<tac> …). Modeled like a notation, but the node is
// a tactic (so the walk labels it a Function).
static void h_tactic_notation(RP *rp) {
    RocqToken n = rp_peek(rp);
    if (tok_eq(n, "Notation")) {
        rp_next(rp);
        h_notation(rp, RSYM_TACTIC);
    } else {
        skip_to_dot(rp);
    }
}

// Coercion <name> : A >-> B. Emits a coercion node with exactly two ident leaves
// (A then B); the walk turns it into an A-implements/coerces-to-B edge.
static void h_coercion(RP *rp) {
    RocqToken a = {0};
    RocqToken b = {0};
    bool have_a = false;
    bool have_b = false;
    bool seen_colon = false;
    bool seen_arrow = false;
    for (;;) {
        RocqToken u = rp_next(rp);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            break;
        }
        if (tok_is_colon(u)) {
            seen_colon = true;
            continue;
        }
        if (u.kind == ROCQ_TOK_SYMBOL && u.len >= 2 && u.text[0] == '>') {
            seen_arrow = true; // the ">->" coercion arrow
            continue;
        }
        if (seen_colon && u.kind == ROCQ_TOK_IDENT) {
            if (!seen_arrow) {
                if (!have_a) {
                    a = u;
                    have_a = true;
                }
            } else if (!have_b) {
                b = u;
                have_b = true;
            }
        }
    }
    if (have_a && have_b) {
        rocq_tb_open(rp->tb, RSYM_COERCION, RPROD_NONE);
        tb_leaf_tok(rp, ident_sym_of(a), a);
        tb_leaf_tok(rp, ident_sym_of(b), b);
        rocq_tb_close(rp->tb);
    }
}

// Instance: name (child 0), the instantiated class head (child 1, the first
// top-level identifier after the leading ':'), then a `term` of body references.
static void h_instance(RP *rp) {
    RocqToken t = rp_peek(rp);
    while (t.kind != ROCQ_TOK_IDENT && t.kind != ROCQ_TOK_DOT && t.kind != ROCQ_TOK_EOF) {
        rp_next(rp);
        t = rp_peek(rp);
    }
    if (t.kind != ROCQ_TOK_IDENT) {
        skip_to_dot(rp);
        return;
    }
    rocq_tb_open(rp->tb, RSYM_INSTANCE, RPROD_INSTANCE);
    RocqToken name = rp_next(rp);
    tb_leaf_tok(rp, RSYM_IDENT, name);

    int depth = 0;
    bool want_class = false;
    bool have_class = false;
    bool term_open = false;
    for (;;) {
        RocqToken u = rp_next(rp);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            break;
        }
        if (!have_class) {
            if (u.kind == ROCQ_TOK_LPAREN || u.kind == ROCQ_TOK_LBRACE ||
                u.kind == ROCQ_TOK_LBRACK) {
                depth++;
                continue;
            }
            if (u.kind == ROCQ_TOK_RPAREN || u.kind == ROCQ_TOK_RBRACE ||
                u.kind == ROCQ_TOK_RBRACK) {
                if (depth > 0) {
                    depth--;
                }
                continue;
            }
            if (depth == 0 && !want_class && tok_is_colon(u)) {
                want_class = true;
                continue;
            }
            if (want_class && depth == 0 && u.kind == ROCQ_TOK_IDENT) {
                tb_leaf_tok(rp, ident_sym_of(u), u); // class head @ child 1
                have_class = true;
            }
            continue;
        }
        body_token(rp, u, &term_open);
    }
    close_term(rp, &term_open);
    rocq_tb_close(rp->tb);
}

// Require [Import|Export] A.B C …: a require node whose leaves are logical module
// paths (no name field ⇒ each leaf is a complete path).
static void h_require(RP *rp) {
    rocq_tb_open(rp->tb, RSYM_REQUIRE, RPROD_NONE);
    for (;;) {
        RocqToken u = rp_next(rp);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            break;
        }
        if (u.kind != ROCQ_TOK_IDENT) {
            continue;
        }
        if (tok_eq(u, "Import") || tok_eq(u, "Export")) {
            continue;
        }
        tb_leaf_tok(rp, ident_sym_of(u), u);
    }
    rocq_tb_close(rp->tb); // dropped if empty
}

// From X Require [Import|Export] Y Z: a require node whose `name` field is the
// prefix X; the walk prepends it to each remaining leaf (X.Y, X.Z).
static void h_from(RP *rp) {
    RocqToken pfx = rp_next(rp);
    if (pfx.kind != ROCQ_TOK_IDENT) {
        if (pfx.kind != ROCQ_TOK_DOT && pfx.kind != ROCQ_TOK_EOF) {
            skip_to_dot(rp);
        }
        return;
    }
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
    rocq_tb_open(rp->tb, RSYM_REQUIRE, RPROD_NAME);
    tb_leaf_tok(rp, ident_sym_of(pfx), pfx); // prefix @ child 0 (name field)
    for (;;) {
        RocqToken u = rp_next(rp);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            break;
        }
        if (u.kind != ROCQ_TOK_IDENT) {
            continue;
        }
        if (tok_eq(u, "Import") || tok_eq(u, "Export")) {
            continue;
        }
        tb_leaf_tok(rp, ident_sym_of(u), u);
    }
    rocq_tb_close(rp->tb);
}

// Import / Export <module> (standalone — opens an already-required module).
static void h_open_import(RP *rp) {
    rocq_tb_open(rp->tb, RSYM_IMPORT, RPROD_NONE);
    for (;;) {
        RocqToken u = rp_next(rp);
        if (u.kind == ROCQ_TOK_DOT || u.kind == ROCQ_TOK_EOF) {
            break;
        }
        if (u.kind == ROCQ_TOK_IDENT) {
            tb_leaf_tok(rp, ident_sym_of(u), u);
        }
    }
    rocq_tb_close(rp->tb);
}

// Proof. — opens a proof node that captures tactic tokens until a terminator.
static void h_proof(RP *rp) {
    rocq_tb_open(rp->tb, RSYM_PROOF, RPROD_NONE);
    rp->proof_open = true;
    skip_to_dot(rp); // consume the `Proof[ using/with …].` header
}

static bool is_proof_terminator(RocqToken t) {
    return t.kind == ROCQ_TOK_IDENT &&
           (tok_eq(t, "Qed") || tok_eq(t, "Defined") || tok_eq(t, "Admitted") ||
            tok_eq(t, "Abort") || tok_eq(t, "Save"));
}

// Returns true if `kw` was dispatched as a known command.
static bool dispatch_keyword(RP *rp, RocqToken kw) {
    if (tok_eq(kw, "Definition") || tok_eq(kw, "Let") || tok_eq(kw, "Example") ||
        tok_eq(kw, "Fixpoint") || tok_eq(kw, "CoFixpoint") || tok_eq(kw, "Function")) {
        h_define(rp, RSYM_DEFINITION);
        return true;
    }
    if (tok_eq(kw, "Theorem") || tok_eq(kw, "Lemma") || tok_eq(kw, "Corollary") ||
        tok_eq(kw, "Proposition") || tok_eq(kw, "Remark") || tok_eq(kw, "Fact") ||
        tok_eq(kw, "Property")) {
        h_define(rp, RSYM_THEOREM);
        return true;
    }
    if (tok_eq(kw, "Instance")) {
        h_instance(rp);
        return true;
    }
    if (tok_eq(kw, "Inductive") || tok_eq(kw, "CoInductive") || tok_eq(kw, "Variant")) {
        h_type(rp, RP_TYPE_INDUCTIVE, RSYM_INDUCTIVE);
        return true;
    }
    if (tok_eq(kw, "Record") || tok_eq(kw, "Structure")) {
        h_type(rp, RP_TYPE_RECORD, RSYM_RECORD);
        return true;
    }
    if (tok_eq(kw, "Class")) {
        h_type(rp, RP_TYPE_RECORD, RSYM_CLASS);
        return true;
    }
    if (tok_eq(kw, "Module")) {
        h_module(rp, RSYM_MODULE, false);
        return true;
    }
    if (tok_eq(kw, "Section")) {
        h_module(rp, RSYM_SECTION, true);
        return true;
    }
    if (tok_eq(kw, "End")) {
        rocq_tb_close(rp->tb); // close the innermost open module/section
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
        h_notation(rp, RSYM_NOTATION);
        return true;
    }
    if (tok_eq(kw, "Tactic")) {
        h_tactic_notation(rp);
        return true;
    }
    if (tok_eq(kw, "Coercion")) {
        h_coercion(rp);
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
        tok_eq(kw, "Abort") || tok_eq(kw, "Save") || tok_eq(kw, "Goal")) {
        // Stray proof terminator / anonymous goal outside a captured proof.
        skip_to_dot(rp);
        return true;
    }
    return false;
}

// Consume attributes (#[ … ]) and modifier keywords so the real command keyword
// can be dispatched. Returns the keyword token, or an EOF token at end of input.
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
        if (t.kind == ROCQ_TOK_SYMBOL && t.text[0] == '#') { // a SYMBOL token always has len >= 1
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
        if (tok_eq(t, "Local") || tok_eq(t, "Global") || tok_eq(t, "Polymorphic") ||
            tok_eq(t, "Monomorphic") || tok_eq(t, "Program") || tok_eq(t, "Cumulative") ||
            tok_eq(t, "NonCumulative") || tok_eq(t, "Private") || tok_eq(t, "Reserved") ||
            tok_eq(t, "Existing") || tok_eq(t, "Canonical")) {
            rp_next(rp);
            continue;
        }
        return rp_next(rp);
    }
}

void *rocq_parse_to_tree(const char *source, int source_len) {
    RP rp = {0};
    rocq_lex_init(&rp.lx, source, source_len);
    rp.tb = rocq_tb_new(source, source_len);
    if (!rp.tb) {
        return NULL;
    }

    for (;;) {
        if (rp.proof_open) {
            RocqToken t = rp_next(&rp);
            if (t.kind == ROCQ_TOK_EOF) {
                break; // rocq_tb_finish closes the dangling proof
            }
            if (is_proof_terminator(t)) {
                rocq_tb_close(rp.tb); // close PROOF
                rp.proof_open = false;
                skip_to_dot(&rp); // consume `Qed.`
                continue;
            }
            RocqSymbol s = leaf_sym_of(t);
            if (s != RSYM_END) {
                tb_leaf_tok(&rp, s, t);
            }
            continue;
        }
        RocqToken kw = read_command_keyword(&rp);
        if (kw.kind == ROCQ_TOK_EOF) {
            break;
        }
        if (kw.kind == ROCQ_TOK_IDENT && dispatch_keyword(&rp, kw)) {
            continue;
        }
        skip_to_dot(&rp); // unrecognized command — record no node
    }

    void *tree = rocq_tb_finish(rp.tb); // closes any unbalanced module/proof frames
    rocq_tb_free(rp.tb);
    return tree;
}
