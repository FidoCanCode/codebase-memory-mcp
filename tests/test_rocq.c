/*
 * test_rocq.c — Tests for the original hand-written Rocq (.v) front-end.
 *
 * Exercises cbm_extract_file() on Rocq snippets, verifying definitions
 * (Definition/Theorem/Inductive/Record/Module/...), Require imports, and
 * proof-dependency calls — plus the lexer hazards (nested comments, doubled
 * quotes, the command-terminating dot vs qualified-name dots).
 */
#include "test_framework.h"
#include "cbm.h"
#include "arena.h"
#include "rocq/rocq_project.h"
#include "rocq/rocq_notation.h"

#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────── */

static CBMFileResult *rocq(const char *src, const char *path) {
    return cbm_extract_file(src, (int)strlen(src), CBM_LANG_ROCQ, "t", path, 0, NULL, NULL);
}

static int has_def(CBMFileResult *r, const char *label, const char *name) {
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].label, label) == 0 && strcmp(r->defs.items[i].name, name) == 0)
            return 1;
    }
    return 0;
}

static int has_def_any(CBMFileResult *r, const char *name) {
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].name, name) == 0)
            return 1;
    }
    return 0;
}

static CBMDefinition *find_def(CBMFileResult *r, const char *label, const char *name) {
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].label, label) == 0 && strcmp(r->defs.items[i].name, name) == 0)
            return &r->defs.items[i];
    }
    return NULL;
}

static int has_call(CBMFileResult *r, const char *callee) {
    for (int i = 0; i < r->calls.count; i++) {
        if (r->calls.items[i].callee_name && strstr(r->calls.items[i].callee_name, callee) != NULL)
            return 1;
    }
    return 0;
}

/* A call to `callee` whose enclosing definition QN contains `owner`. */
static int has_call_from(CBMFileResult *r, const char *callee, const char *owner) {
    for (int i = 0; i < r->calls.count; i++) {
        const CBMCall *c = &r->calls.items[i];
        if (c->callee_name && strcmp(c->callee_name, callee) == 0 && c->enclosing_func_qn &&
            strstr(c->enclosing_func_qn, owner) != NULL)
            return 1;
    }
    return 0;
}

static int has_import(CBMFileResult *r, const char *path_substr) {
    for (int i = 0; i < r->imports.count; i++) {
        if (r->imports.items[i].module_path &&
            strstr(r->imports.items[i].module_path, path_substr) != NULL)
            return 1;
    }
    return 0;
}

/* ── Definitions ───────────────────────────────────────────────── */

TEST(rocq_definition_and_body_call) {
    CBMFileResult *r = rocq("Definition double (n : nat) := add n n.\n", "demo.v");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "double"));
    ASSERT(has_call(r, "add")); /* body reference becomes a CALLS candidate */
    cbm_free_result(r);
    PASS();
}

TEST(rocq_file_module_node) {
    /* Every .v file gets a Module node named after its module leaf. */
    CBMFileResult *r = rocq("Definition x := 0.\n", "theories/Base.v");
    ASSERT_NOT_NULL(r);
    ASSERT(has_def(r, "Module", "Base"));
    cbm_free_result(r);
    PASS();
}

TEST(rocq_theorem_proof_dependencies) {
    CBMFileResult *r = rocq("Lemma helper : True.\n"
                            "Proof. exact I. Qed.\n"
                            "Theorem main_thm : True.\n"
                            "Proof. apply helper. Qed.\n",
                            "demo.v");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "helper"));
    ASSERT(has_def(r, "Function", "main_thm"));
    /* The headline feature: the proof of main_thm depends on helper. */
    ASSERT(has_call_from(r, "helper", "main_thm"));
    /* Tactic keywords are filtered out — they are not call targets. */
    ASSERT_FALSE(has_call(r, "apply"));
    ASSERT_FALSE(has_call(r, "exact"));
    cbm_free_result(r);
    PASS();
}

TEST(rocq_inductive_constructors) {
    CBMFileResult *r = rocq("Inductive color := Red | Green | Blue.\n", "demo.v");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Type", "color"));
    ASSERT(has_def(r, "Method", "Red"));
    ASSERT(has_def(r, "Method", "Green"));
    ASSERT(has_def(r, "Method", "Blue"));
    /* Constructors are nested under the inductive type. */
    CBMDefinition *red = find_def(r, "Method", "Red");
    ASSERT_NOT_NULL(red);
    ASSERT_NOT_NULL(red->parent_class);
    ASSERT(strstr(red->parent_class, "color") != NULL);
    cbm_free_result(r);
    PASS();
}

TEST(rocq_inductive_constructors_with_args) {
    CBMFileResult *r = rocq("Inductive tree :=\n"
                            "  | Leaf\n"
                            "  | Node (l : tree) (r : tree).\n",
                            "demo.v");
    ASSERT_NOT_NULL(r);
    ASSERT(has_def(r, "Type", "tree"));
    ASSERT(has_def(r, "Method", "Leaf"));
    ASSERT(has_def(r, "Method", "Node"));
    /* The constructor argument types must NOT be mistaken for constructors. */
    ASSERT_FALSE(has_def(r, "Method", "l"));
    cbm_free_result(r);
    PASS();
}

TEST(rocq_record_fields) {
    CBMFileResult *r = rocq("Record point := mkPoint { px : nat; py : nat }.\n", "demo.v");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Type", "point"));
    ASSERT(has_def(r, "Method", "px"));
    ASSERT(has_def(r, "Method", "py"));
    cbm_free_result(r);
    PASS();
}

TEST(rocq_fixpoint_mutual) {
    CBMFileResult *r = rocq("Fixpoint is_even n := match n with 0 => true | S k => is_odd k end\n"
                            "with is_odd n := match n with 0 => false | S k => is_even k end.\n",
                            "demo.v");
    ASSERT_NOT_NULL(r);
    ASSERT(has_def(r, "Function", "is_even"));
    ASSERT(has_def(r, "Function", "is_odd"));
    cbm_free_result(r);
    PASS();
}

TEST(rocq_module_scope_qn) {
    CBMFileResult *r = rocq("Module M.\n"
                            "  Definition x := 0.\n"
                            "End M.\n"
                            "Definition y := 1.\n",
                            "demo.v");
    ASSERT_NOT_NULL(r);
    ASSERT(has_def(r, "Module", "M"));
    /* x is scoped under M; y is back at file scope. */
    CBMDefinition *x = find_def(r, "Function", "x");
    ASSERT_NOT_NULL(x);
    ASSERT(strstr(x->qualified_name, ".M.x") != NULL);
    CBMDefinition *y = find_def(r, "Function", "y");
    ASSERT_NOT_NULL(y);
    ASSERT_NULL(strstr(y->qualified_name, ".M."));
    cbm_free_result(r);
    PASS();
}

TEST(rocq_section_scope) {
    CBMFileResult *r = rocq("Section S.\n"
                            "  Definition helper := 0.\n"
                            "End S.\n",
                            "demo.v");
    ASSERT_NOT_NULL(r);
    ASSERT(has_def(r, "Module", "S"));
    ASSERT(has_def(r, "Function", "helper"));
    cbm_free_result(r);
    PASS();
}

TEST(rocq_ltac_parameter_axiom_notation) {
    CBMFileResult *r = rocq("Parameter foo : nat.\n"
                            "Axiom bar : True.\n"
                            "Ltac myauto := auto.\n"
                            "Notation \"x +++ y\" := (plus x y).\n",
                            "demo.v");
    ASSERT_NOT_NULL(r);
    ASSERT(has_def(r, "Variable", "foo"));
    ASSERT(has_def(r, "Variable", "bar"));
    ASSERT(has_def(r, "Function", "myauto"));
    ASSERT(has_def_any(r, "x +++ y")); /* notation node named after its string */
    cbm_free_result(r);
    PASS();
}

/* ── Imports ───────────────────────────────────────────────────── */

TEST(rocq_require_import_forms) {
    CBMFileResult *r = rocq("Require Import Coq.Lists.List.\n"
                            "Require Export Coq.Arith.PeanoNat.\n"
                            "From Stdlib Require Import Bool ZArith.\n",
                            "demo.v");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_import(r, "Coq.Lists.List"));
    ASSERT(has_import(r, "Coq.Arith.PeanoNat"));
    ASSERT(has_import(r, "Stdlib.Bool"));
    ASSERT(has_import(r, "Stdlib.ZArith"));
    cbm_free_result(r);
    PASS();
}

/* ── Lexer hazards ─────────────────────────────────────────────── */

TEST(rocq_nested_comments) {
    CBMFileResult *r = rocq("(* outer (* inner *) still outer *)\n"
                            "Definition foo := 0.\n",
                            "demo.v");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "foo"));
    cbm_free_result(r);
    PASS();
}

TEST(rocq_string_inside_comment) {
    /* A "*)" that lives inside a string inside a comment must not close it. */
    CBMFileResult *r = rocq("(* a string \"*)\" inside the comment *)\n"
                            "Definition bar := 1.\n",
                            "demo.v");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "bar"));
    cbm_free_result(r);
    PASS();
}

TEST(rocq_doubled_quote_string) {
    /* The doubled-quote escape must not terminate the string early. */
    CBMFileResult *r = rocq("Definition s := \"a \"\"b\"\" c\".\n"
                            "Definition after := 0.\n",
                            "demo.v");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "s"));
    ASSERT(has_def(r, "Function", "after"));
    cbm_free_result(r);
    PASS();
}

TEST(rocq_qualified_name_call) {
    /* A qualified callee survives as a single dotted token, not split by the
     * command-terminator rule. */
    CBMFileResult *r = rocq("Definition f := Nat.add 1 2.\n", "demo.v");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "f"));
    ASSERT(has_call(r, "Nat.add"));
    cbm_free_result(r);
    PASS();
}

/* ── dynamic notation resolution ───────────────────────────────── */

TEST(rocq_notation_use_resolves) {
    CBMFileResult *r = rocq("Notation \"x +++ y\" := (myadd x y).\n"
                            "Definition f (a b : nat) := a +++ b.\n",
                            "demo.v");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "f"));
    /* The notation use `a +++ b` resolves to myadd via the dynamic table. */
    ASSERT(has_call_from(r, "myadd", "f"));
    cbm_free_result(r);
    PASS();
}

TEST(rocq_infix_notation_resolves) {
    CBMFileResult *r = rocq("Infix \"<*>\" := mymul.\n"
                            "Definition g (a b : nat) := a <*> b.\n",
                            "demo.v");
    ASSERT_NOT_NULL(r);
    ASSERT(has_call_from(r, "mymul", "g"));
    cbm_free_result(r);
    PASS();
}

/* The dynamic, file-order property: a notation resolves uses that FOLLOW its
 * declaration, but not uses that precede it. */
TEST(rocq_notation_file_order) {
    CBMFileResult *r = rocq("Definition early (a b : nat) := a @@@ b.\n"
                            "Notation \"x @@@ y\" := (late_add x y).\n"
                            "Definition later (a b : nat) := a @@@ b.\n",
                            "demo.v");
    ASSERT_NOT_NULL(r);
    ASSERT(has_call_from(r, "late_add", "later"));      /* declared before use */
    ASSERT_FALSE(has_call_from(r, "late_add", "early")); /* used before declared */
    cbm_free_result(r);
    PASS();
}

/* ── dune coq.theory logical→physical resolution ───────────────── */

TEST(rocq_projmap_resolves_dune_theory) {
    RocqProjMap m;
    rocq_projmap_init(&m);
    const char *dune = "(coq.theory\n (name MyDev)\n (package coq-mydev))\n";
    rocq_projmap_add_dune(&m, "theories", dune, (int)strlen(dune));

    char out[256];
    ASSERT(rocq_projmap_resolve(&m, "MyDev.Base", out, sizeof(out)));
    ASSERT_STR_EQ(out, "theories/Base.v");
    /* Subdirectories qualify the module name. */
    ASSERT(rocq_projmap_resolve(&m, "MyDev.Sub.Mod", out, sizeof(out)));
    ASSERT_STR_EQ(out, "theories/Sub/Mod.v");
    /* A different logical prefix doesn't match. */
    ASSERT_FALSE(rocq_projmap_resolve(&m, "Other.X", out, sizeof(out)));

    rocq_projmap_free(&m);
    PASS();
}

TEST(rocq_projmap_dune_dotted_name_and_root) {
    RocqProjMap m;
    rocq_projmap_init(&m);
    const char *d1 = "(coq.theory (name My.Lib))";
    rocq_projmap_add_dune(&m, "src", d1, (int)strlen(d1));
    /* A dune-project at the repo root (empty dir). */
    const char *d2 = "(coq.theory (name Root))";
    rocq_projmap_add_dune(&m, "", d2, (int)strlen(d2));

    char out[256];
    ASSERT(rocq_projmap_resolve(&m, "My.Lib.Core", out, sizeof(out)));
    ASSERT_STR_EQ(out, "src/Core.v");
    ASSERT(rocq_projmap_resolve(&m, "Root.Top", out, sizeof(out)));
    ASSERT_STR_EQ(out, "Top.v");

    rocq_projmap_free(&m);
    PASS();
}

/* ── typeclasses ───────────────────────────────────────────────── */

TEST(rocq_class_is_interface_instance_implements) {
    CBMFileResult *r = rocq("Class Eq (a : Type) := { eqb : a -> a -> bool }.\n"
                            "Instance nat_eq : Eq nat := { eqb := Nat.eqb }.\n",
                            "demo.v");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    /* A typeclass is modeled as an Interface; its method is a child. */
    ASSERT(has_def(r, "Interface", "Eq"));
    ASSERT(has_def(r, "Method", "eqb"));
    /* The instance is a Function whose base class is the typeclass it instantiates. */
    CBMDefinition *inst = find_def(r, "Function", "nat_eq");
    ASSERT_NOT_NULL(inst);
    ASSERT_NOT_NULL(inst->base_classes);
    int saw = 0;
    for (const char **b = inst->base_classes; *b; b++) {
        if (strcmp(*b, "Eq") == 0) {
            saw = 1;
        }
    }
    ASSERT(saw);
    cbm_free_result(r);
    PASS();
}

/* The class head is found past binders (the binder colon must not be mistaken
 * for the class-introducing colon). */
TEST(rocq_instance_class_past_binders) {
    CBMFileResult *r = rocq("Instance li (A : Type) : Container (list A) := {}.\n", "demo.v");
    ASSERT_NOT_NULL(r);
    CBMDefinition *inst = find_def(r, "Function", "li");
    ASSERT_NOT_NULL(inst);
    ASSERT_NOT_NULL(inst->base_classes);
    ASSERT_STR_EQ(inst->base_classes[0], "Container");
    cbm_free_result(r);
    PASS();
}

/* ── cross-file notation primitives ────────────────────────────── */

TEST(rocq_scan_extracts_notations_and_requires) {
    CBMArena a;
    cbm_arena_init(&a);
    const char *src = "Notation \"x +++ y\" := (myadd x y).\n"
                      "From MyDev Require Import Base Extra.\n"
                      "Require Import Coq.Lists.List.\n";
    RocqFileScan fs;
    rocq_scan_file(&a, src, (int)strlen(src), &fs);

    int found_op = 0;
    for (int i = 0; i < fs.notation_count; i++) {
        if (strcmp(fs.notations[i].op, "+++") == 0 && strcmp(fs.notations[i].target, "myadd") == 0) {
            found_op = 1;
        }
    }
    ASSERT(found_op);

    int found_base = 0, found_list = 0;
    for (int i = 0; i < fs.require_count; i++) {
        if (strcmp(fs.requires[i], "MyDev.Base") == 0) {
            found_base = 1;
        }
        if (strcmp(fs.requires[i], "Coq.Lists.List") == 0) {
            found_list = 1;
        }
    }
    ASSERT(found_base);
    ASSERT(found_list);

    cbm_arena_destroy(&a);
    PASS();
}

TEST(rocq_seeddb_basic) {
    RocqSeedDB *db = rocq_seeddb_new();
    rocq_seeddb_add(db, "f.v", "@@", "tgt");
    rocq_seeddb_add(db, "f.v", "@@", "tgt"); /* duplicate ignored */
    rocq_seeddb_add(db, "f.v", "##", "tgt2");

    const RocqNotationEntry *e = NULL;
    ASSERT_EQ(rocq_seeddb_lookup(db, "f.v", &e), 2);
    ASSERT_NOT_NULL(e);
    ASSERT_EQ(rocq_seeddb_lookup(db, "none.v", &e), 0);
    rocq_seeddb_free(db);
    PASS();
}

TEST(rocq_logical_for_path) {
    RocqProjMap m;
    rocq_projmap_init(&m);
    const char *d = "(coq.theory (name MyDev))";
    rocq_projmap_add_dune(&m, "theories", d, (int)strlen(d));

    char out[256];
    ASSERT(rocq_projmap_logical_for_path(&m, "theories/Base.v", out, sizeof(out)));
    ASSERT_STR_EQ(out, "MyDev.Base");
    ASSERT(rocq_projmap_logical_for_path(&m, "theories/Sub/Mod.v", out, sizeof(out)));
    ASSERT_STR_EQ(out, "MyDev.Sub.Mod");
    /* A path outside any theory root has no logical name. */
    ASSERT_FALSE(rocq_projmap_logical_for_path(&m, "other/x.v", out, sizeof(out)));
    rocq_projmap_free(&m);
    PASS();
}

/* ── Suite ─────────────────────────────────────────────────────── */

SUITE(rocq) {
    RUN_TEST(rocq_definition_and_body_call);
    RUN_TEST(rocq_file_module_node);
    RUN_TEST(rocq_theorem_proof_dependencies);
    RUN_TEST(rocq_inductive_constructors);
    RUN_TEST(rocq_inductive_constructors_with_args);
    RUN_TEST(rocq_record_fields);
    RUN_TEST(rocq_fixpoint_mutual);
    RUN_TEST(rocq_module_scope_qn);
    RUN_TEST(rocq_section_scope);
    RUN_TEST(rocq_ltac_parameter_axiom_notation);
    RUN_TEST(rocq_require_import_forms);
    RUN_TEST(rocq_nested_comments);
    RUN_TEST(rocq_string_inside_comment);
    RUN_TEST(rocq_doubled_quote_string);
    RUN_TEST(rocq_qualified_name_call);
    RUN_TEST(rocq_notation_use_resolves);
    RUN_TEST(rocq_infix_notation_resolves);
    RUN_TEST(rocq_notation_file_order);
    RUN_TEST(rocq_projmap_resolves_dune_theory);
    RUN_TEST(rocq_projmap_dune_dotted_name_and_root);
    RUN_TEST(rocq_scan_extracts_notations_and_requires);
    RUN_TEST(rocq_seeddb_basic);
    RUN_TEST(rocq_logical_for_path);
    RUN_TEST(rocq_class_is_interface_instance_implements);
    RUN_TEST(rocq_instance_class_past_binders);
}
