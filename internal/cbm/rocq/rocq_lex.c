// SPDX-License-Identifier: MIT
//
// rocq_lex.c — see rocq_lex.h. Original hand-written scanner for Rocq.
#include "rocq/rocq_lex.h"

static bool is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

bool rocq_is_ident_start(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

static bool is_ident_cont(unsigned char c) {
    // Rocq identifiers admit primes and trailing digits (x', IHn0').
    return rocq_is_ident_start(c) || (c >= '0' && c <= '9') || c == '\'';
}

static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

// Operator bytes that we coalesce into a single SYMBOL token. '.' is handled
// separately because of the command-terminator rule; brackets get their own
// token kinds.
static bool is_op_char(unsigned char c) {
    switch (c) {
    case ':':
    case '=':
    case '<':
    case '>':
    case '+':
    case '-':
    case '*':
    case '/':
    case '\\':
    case '|':
    case '&':
    case '~':
    case '!':
    case '?':
    case '%':
    case '^':
    case '@':
    case ',':
    case ';':
    case '$':
    case '#':
        return true;
    default:
        return false;
    }
}

void rocq_lex_init(RocqLexer *lx, const char *src, int len) {
    lx->src = src;
    lx->len = len;
    lx->pos = 0;
    lx->line = 1;
    lx->soft_error = false;
}

// Advance past a string literal whose opening quote is at lx->pos. Handles the
// Rocq doubled-quote escape ("" inside a string is a literal quote) and counts
// embedded newlines. Leaves lx->pos just past the closing quote.
static void skip_string(RocqLexer *lx) {
    lx->pos++; // opening quote
    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos];
        if (c == '"') {
            if (lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '"') {
                lx->pos += 2; // escaped quote
                continue;
            }
            lx->pos++; // closing quote
            return;
        }
        if (c == '\n') {
            lx->line++;
        }
        lx->pos++;
    }
    lx->soft_error = true; // unterminated string
}

// Advance past a block comment whose opening "(*" is at lx->pos. Comments nest,
// and string literals inside a comment are skipped so that a "*)" appearing
// within a string does not close the comment early.
static void skip_comment(RocqLexer *lx) {
    lx->pos += 2; // past "(*"
    int depth = 1;
    while (lx->pos < lx->len && depth > 0) {
        char c = lx->src[lx->pos];
        if (c == '(' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '*') {
            depth++;
            lx->pos += 2;
        } else if (c == '*' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == ')') {
            depth--;
            lx->pos += 2;
        } else if (c == '"') {
            skip_string(lx);
        } else {
            if (c == '\n') {
                lx->line++;
            }
            lx->pos++;
        }
    }
    if (depth > 0) {
        lx->soft_error = true; // unterminated comment
    }
}

// Skip whitespace and comments; returns with lx->pos at the next token or EOF.
static void skip_trivia(RocqLexer *lx) {
    for (;;) {
        if (lx->pos >= lx->len) {
            return;
        }
        char c = lx->src[lx->pos];
        if (is_space((unsigned char)c)) {
            if (c == '\n') {
                lx->line++;
            }
            lx->pos++;
            continue;
        }
        if (c == '(' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '*') {
            skip_comment(lx);
            continue;
        }
        return;
    }
}

// Is the byte at `at` the start of trivia (whitespace, EOF, or a comment)?
// Used by the dot-terminator rule: a '.' ends a command only when the next
// byte is "blank" in this sense.
static bool blank_or_eof_at(const RocqLexer *lx, int at) {
    if (at >= lx->len) {
        return true;
    }
    char c = lx->src[at];
    if (is_space((unsigned char)c)) {
        return true;
    }
    if (c == '(' && at + 1 < lx->len && lx->src[at + 1] == '*') {
        return true;
    }
    return false;
}

static RocqToken make_tok(const RocqLexer *lx, RocqTokenKind kind, int start, int line) {
    RocqToken t;
    t.kind = kind;
    t.text = lx->src + start;
    t.len = lx->pos - start;
    t.start_byte = start;
    t.line = line;
    return t;
}

RocqToken rocq_lex_next(RocqLexer *lx) {
    skip_trivia(lx);
    int start = lx->pos;
    int line = lx->line;

    if (lx->pos >= lx->len) {
        return make_tok(lx, ROCQ_TOK_EOF, start, line);
    }

    char c = lx->src[lx->pos];

    // Identifier or dotted qualified name.
    if (rocq_is_ident_start((unsigned char)c)) {
        lx->pos++;
        while (lx->pos < lx->len && is_ident_cont((unsigned char)lx->src[lx->pos])) {
            lx->pos++;
        }
        // Fold qualifier dots: "Mod.ident" stays one token, but a trailing
        // command dot ("foo.") or projection dot ("x.(p)") does not fold.
        while (lx->pos + 1 < lx->len && lx->src[lx->pos] == '.' &&
               rocq_is_ident_start((unsigned char)lx->src[lx->pos + 1])) {
            lx->pos++; // the '.'
            while (lx->pos < lx->len && is_ident_cont((unsigned char)lx->src[lx->pos])) {
                lx->pos++;
            }
        }
        return make_tok(lx, ROCQ_TOK_IDENT, start, line);
    }

    // Numeric literal.
    if (is_digit((unsigned char)c)) {
        lx->pos++;
        while (lx->pos < lx->len && is_digit((unsigned char)lx->src[lx->pos])) {
            lx->pos++;
        }
        return make_tok(lx, ROCQ_TOK_NUMBER, start, line);
    }

    // String literal.
    if (c == '"') {
        skip_string(lx);
        return make_tok(lx, ROCQ_TOK_STRING, start, line);
    }

    // Brackets.
    switch (c) {
    case '(':
        lx->pos++;
        return make_tok(lx, ROCQ_TOK_LPAREN, start, line);
    case ')':
        lx->pos++;
        return make_tok(lx, ROCQ_TOK_RPAREN, start, line);
    case '{':
        lx->pos++;
        return make_tok(lx, ROCQ_TOK_LBRACE, start, line);
    case '}':
        lx->pos++;
        return make_tok(lx, ROCQ_TOK_RBRACE, start, line);
    case '[':
        lx->pos++;
        return make_tok(lx, ROCQ_TOK_LBRACK, start, line);
    case ']':
        lx->pos++;
        return make_tok(lx, ROCQ_TOK_RBRACK, start, line);
    default:
        break;
    }

    // Dot: terminator, ellipsis, or a glued symbol (projection ".(").
    if (c == '.') {
        if (lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '.') {
            while (lx->pos < lx->len && lx->src[lx->pos] == '.') {
                lx->pos++; // consume the whole dot run as ellipsis
            }
            return make_tok(lx, ROCQ_TOK_SYMBOL, start, line);
        }
        if (blank_or_eof_at(lx, lx->pos + 1)) {
            lx->pos++;
            return make_tok(lx, ROCQ_TOK_DOT, start, line);
        }
        lx->pos++; // a dot glued to a following non-blank token
        return make_tok(lx, ROCQ_TOK_SYMBOL, start, line);
    }

    // Operator run.
    if (is_op_char((unsigned char)c)) {
        lx->pos++;
        while (lx->pos < lx->len && is_op_char((unsigned char)lx->src[lx->pos])) {
            lx->pos++;
        }
        return make_tok(lx, ROCQ_TOK_SYMBOL, start, line);
    }

    // Any other single byte (rare punctuation) — emit as a symbol so the
    // stream always advances.
    lx->pos++;
    return make_tok(lx, ROCQ_TOK_SYMBOL, start, line);
}
