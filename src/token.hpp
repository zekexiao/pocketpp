#pragma once
#include <string>

namespace pocketpp {

enum class TT {
    NUMBER, STRING, IDENT,
    PLUS, MINUS, STAR, SLASH, PERCENT,
    STAR_STAR,        // **
    DOT_DOT,          // ..
    PIPE, AMP, CARET, TILDE,
    LSHIFT, RSHIFT,   // << >>
    EQ_EQ, BANG_EQ, LT, LT_EQ, GT, GT_EQ,
    BANG,
    ASSIGN,           // =
    PLUS_EQ, MINUS_EQ, STAR_EQ, SLASH_EQ, PERCENT_EQ,
    STAR_STAR_EQ, AMP_EQ, PIPE_EQ, CARET_EQ, LSHIFT_EQ, RSHIFT_EQ,
    DOT, COMMA, COLON, SEMICOLON,
    LPAREN, RPAREN, LBRACKET, RBRACKET, LBRACE, RBRACE,
    DOTDOT_CONCAT,    // .. used as string concat (same token, context-dependent)
    NEWLINE, EOFILE,
    // Keywords
    DEF, END, FN, RETURN,
    IF, ELIF, ELSE, THEN,
    WHILE, FOR, IN, DO, BREAK,
    CLASS, IS, SELF, SUPER,
    IMPORT, FROM, AS, YIELD,
    KW_TRUE, KW_FALSE, KW_NULL,
    NOT, AND, OR
};

struct Token {
    TT type{TT::EOFILE};
    std::string lexeme;
    double num_val{0};
    int line{1};
};

} // namespace pocketpp
