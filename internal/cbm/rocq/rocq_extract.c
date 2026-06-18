// SPDX-License-Identifier: MIT
//
// rocq_extract.c — see rocq_extract.h. Original Rocq extraction entry point.
#include "rocq/rocq_extract.h"
#include "rocq/rocq_parse.h"

#include "helpers.h"

#include <string.h>

void cbm_rocq_extract_file(CBMArena *a, CBMFileResult *result, const char *source, int source_len,
                           const char *project, const char *rel_path) {
    result->module_qn = cbm_fqn_module(a, project, rel_path);
    result->is_test_file = cbm_is_test_file(rel_path, CBM_LANG_ROCQ);

    // Emit one Module node for the file itself. Its name is the module's leaf
    // segment, which lets `Require Import <...>.<Leaf>` in other files resolve
    // to this node via the import resolver's symbol-name strategy.
    if (result->module_qn && result->module_qn[0]) {
        const char *leaf = result->module_qn;
        const char *dot = strrchr(result->module_qn, '.');
        if (dot) {
            leaf = dot + 1;
        }
        if (leaf[0]) {
            CBMDefinition def = {0};
            def.name = leaf; // points into the arena-owned module_qn
            def.qualified_name = result->module_qn;
            def.label = "Module";
            def.file_path = rel_path ? cbm_arena_strdup(a, rel_path) : NULL;
            def.start_line = 1;
            def.end_line = 1;
            def.complexity = 1;
            cbm_defs_push(&result->defs, a, def);
        }
    }

    /* module_qn is NULL only if the arena couldn't allocate it (OOM); without a
     * base QN there is nothing to anchor definitions to, so skip parsing. */
    if (result->module_qn) {
        rocq_parse_file(a, result, source, source_len, result->module_qn, rel_path);
    }
}
