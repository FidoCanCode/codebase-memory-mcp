// SPDX-License-Identifier: MIT
//
// rocq_lex.h — Token scanner for the Rocq (formerly Coq) Vernacular language.
//
// This is original work: a hand-written byte scanner built from the Rocq
// reference manual and experimentation, not derived from any tree-sitter
// grammar or language server. It tokenizes the surface syntax just enough to
// recognize command boundaries, qualified names, and the lexical hazards that
// make Rocq awkward (nesting block comments, doubled-quote string escapes, and
// the dot-that-may-or-may-not end a command).
#ifndef CBM_ROCQ_LEX_H
#define CBM_ROCQ_LEX_H

#include <stdbool.h>

typedef enum {
    ROCQ_TOK_EOF = 0,
    ROCQ_TOK_IDENT,  // identifier or dotted qualified name (e.g. "x", "Nat.add")
    ROCQ_TOK_DOT,    // a command-terminating dot (followed by blank/EOF/comment)
    ROCQ_TOK_NUMBER, // numeric literal
    ROCQ_TOK_STRING, // "..." literal (raw bytes incl. the quotes)
    ROCQ_TOK_LPAREN,
    ROCQ_TOK_RPAREN,
    ROCQ_TOK_LBRACE,
    ROCQ_TOK_RBRACE,
    ROCQ_TOK_LBRACK,
    ROCQ_TOK_RBRACK,
    ROCQ_TOK_SYMBOL, // run of operator chars, ".(", "..", ":=", "|", "@", ...
} RocqTokenKind;

typedef struct {
    RocqTokenKind kind;
    const char *text; // borrowed pointer into the source buffer (NOT terminated)
    int len;          // byte length of `text`
    int start_byte;
    int line; // 1-based line of the token's first byte
} RocqToken;

typedef struct {
    const char *src;
    int len;
    int pos;
    int line;
    bool soft_error; // set when a comment or string runs off the end of input
} RocqLexer;

// Initialize the scanner over [src, src+len).
void rocq_lex_init(RocqLexer *lx, const char *src, int len);

// Return the next significant token, skipping whitespace and comments.
// At end of input it returns a ROCQ_TOK_EOF token repeatedly.
RocqToken rocq_lex_next(RocqLexer *lx);

// True for a byte that may begin a Rocq identifier (ASCII letter, '_', or any
// continuation byte of a multibyte UTF-8 sequence).
bool rocq_is_ident_start(unsigned char c);

#endif // CBM_ROCQ_LEX_H
