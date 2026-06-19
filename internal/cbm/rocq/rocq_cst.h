// SPDX-License-Identifier: MIT
//
// rocq_cst.h — node-type (symbol), field, and production taxonomy for the
// hand-built Rocq concrete syntax tree.
//
// Original work. Our parser constructs a real tree-sitter TSTree directly (no
// CLI, no generated tables); this header is the single source of truth for the
// symbol ids, field ids, and production ids shared between the hand-authored
// TSLanguage (rocq_ts_language.c) and the tree-building parser. Keeping them in
// one place guarantees the language tables and the constructed tree agree.
#ifndef CBM_ROCQ_CST_H
#define CBM_ROCQ_CST_H

// Node-type symbols. Index 0 is the tree-sitter built-in end symbol.
typedef enum {
    RSYM_END = 0,
    RSYM_SOURCE_FILE,
    RSYM_COMMENT, // extra (block comment)
    // tokens (terminals)
    RSYM_IDENT,
    RSYM_QUALID,
    RSYM_NUMBER,
    RSYM_STRING,
    RSYM_OPERATOR,
    // term layer
    RSYM_TERM, // generic term/expression holding references (idents/qualids/ops)
    // vernacular commands
    RSYM_DEFINITION, // Definition/Let/Example/Fixpoint/CoFixpoint/Function (Function label)
    RSYM_THEOREM,    // Theorem/Lemma/Corollary/... (Function label, proof-bearing)
    RSYM_INDUCTIVE,  // Inductive/CoInductive/Variant (Type)
    RSYM_CONSTRUCTOR,
    RSYM_RECORD, // Record/Structure (Type)
    RSYM_FIELD_DEF,
    RSYM_CLASS,    // typeclass (Interface)
    RSYM_INSTANCE, // typeclass instance (Function)
    RSYM_MODULE,
    RSYM_MODULE_TYPE,
    RSYM_SECTION,
    RSYM_REQUIRE,
    RSYM_IMPORT,
    RSYM_NOTATION,
    RSYM_TACTIC,
    RSYM_ASSUMPTION, // Parameter/Axiom/Variable/Hypothesis/Context
    RSYM_PROOF,
    RSYM_COERCION,
    RSYM_GENERIC, // any other command
    RSYM_COUNT
} RocqSymbol;

// Field ids. Index 0 means "no field". IDs are assigned in ALPHABETICAL order of
// the field name because the tree-sitter runtime's field_id_for_name lookup
// assumes field_names[] is sorted and early-exits on the first name that sorts
// after the query (language.c). Generated grammars emit names sorted; we match.
typedef enum {
    RFLD_NONE = 0,
    RFLD_BODY,    // "body"
    RFLD_CLASS,   // "class"
    RFLD_NAME,    // "name"
    RFLD_PATTERN, // "pattern"
    RFLD_TYPE,    // "type"
    RFLD_COUNT
} RocqField;

// Production ids select a field map (which structural child holds which field).
typedef enum {
    RPROD_NONE = 0,    // no fields
    RPROD_NAME,        // field name @ child 0
    RPROD_NAME_TYPE,   // name @ 0, type @ 1
    RPROD_INSTANCE,    // name @ 0, class @ 1
    RPROD_NOTATION,    // pattern @ 0
    RPROD_COUNT
} RocqProduction;

#endif // CBM_ROCQ_CST_H
