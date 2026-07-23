#pragma once

#include <string>
#include <cstdint>

namespace binar {

enum class TokenType {
    // Literals
    INT_LIT,
    FLOAT_LIT,
    STRING_LIT,
    CHAR_LIT,
    BOOL_LIT,      // true, false
    IDENT,

    // Keywords
    KW_FN,
    KW_TYPE,
    KW_CONST,
    KW_IMPORT,
    KW_IF,
    KW_ELSE,
    KW_FOR,
    KW_WHILE,
    KW_RETURN,
    KW_BREAK,
    KW_CONTINUE,
    KW_RANGE,
    KW_DEFER,
    KW_NIL,
    KW_TRUE,
    KW_FALSE,
    KW_SWITCH,
    KW_CASE,
    KW_DEFAULT,
    KW_ASM,
    KW_VOLATILE,
    KW_IFACE,
    KW_FROM,
    KW_RAISE,

    // Types
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_BOOL,
    TYPE_STRING,
    TYPE_CHAR,
    TYPE_U8,
    TYPE_U16,
    TYPE_U32,
    TYPE_U64,
    TYPE_I8,
    TYPE_I16,
    TYPE_I32,
    TYPE_I64,
    TYPE_ERROR,

    // Operators
    PLUS,           // +
    MINUS,          // -
    STAR,           // *
    SLASH,          // /
    PERCENT,        // %
    AMP,            // &
    PIPE,           // |
    CARET,          // ^
    TILDE,          // ~
    LSHIFT,         // <<
    RSHIFT,         // >>

    ASSIGN,         // =
    EQ,             // ==
    NEQ,            // !=
    LT,             // <
    GT,             // >
    LTE,            // <=
    GTE,            // >=

    AND,            // &&
    OR,             // ||
    NOT,            // !

    PLUS_ASSIGN,    // +=
    MINUS_ASSIGN,   // -=
    STAR_ASSIGN,    // *=
    SLASH_ASSIGN,   // /=
    PERCENT_ASSIGN, // %=
    AMP_ASSIGN,     // &=
    PIPE_ASSIGN,    // |=
    CARET_ASSIGN,   // ^=
    LSHIFT_ASSIGN,  // <<=
    RSHIFT_ASSIGN,  // >>=

    PLUS_PLUS,      // ++
    MINUS_MINUS,    // --

    // Delimiters
    LPAREN,         // (
    RPAREN,         // )
    LBRACE,         // {
    RBRACE,         // }
    LBRACKET,       // [
    RBRACKET,       // ]
    COLON,          // :
    COLON_ASSIGN,   // :=
    SEMICOLON,      // ;
    COMMA,          // ,
    DOT,            // .
    ARROW,          // ->
    AMPERSAND,      // & (also address-of)
    STAR_PTR,       // * (also pointer deref)

    // Special
    NEWLINE,
    EOF_TOKEN,
    INVALID,
};

struct Token {
    TokenType type;
    std::string value;
    int line;
    int column;

    Token() : type(TokenType::INVALID), line(0), column(0) {}
    Token(TokenType type, const std::string& value, int line, int column)
        : type(type), value(value), line(line), column(column) {}
};

const char* token_type_name(TokenType type);

} // namespace binar
