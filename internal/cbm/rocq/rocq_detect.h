// SPDX-License-Identifier: MIT
//
// rocq_detect.h — Rocq-vs-Verilog content disambiguation helper for .v files.
//
// The .v extension is shared by Verilog and Rocq. cbm_disambiguate_v() (discovery
// layer) first applies a cheap line-anchored Verilog structural veto, then — only
// for files that survive it — calls into here.
#ifndef CBM_ROCQ_DETECT_H
#define CBM_ROCQ_DETECT_H

// Parse a bounded prefix of a .v source with the real Vernacular parser and
// return the number of top-level Rocq declarations it recognizes (0 = no Rocq
// evidence in this prefix).
//
// Rather than maintaining a second list of Rocq keywords (which would drift out
// of sync with the parser), this runs the actual parser, which makes it:
//   - bounded : the caller passes only a prefix (same window as the Verilog veto);
//   - lenient : the parser emits a declaration node at the command *head*
//               (e.g. `Definition foo`) before consuming the body, so a term
//               truncated at the prefix boundary still counts; and
//   - drift-free : the parser is the single source of truth for what is Rocq.
int cbm_rocq_count_decls(const char *src, int len);

#endif // CBM_ROCQ_DETECT_H
