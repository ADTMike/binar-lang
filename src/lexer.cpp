#include "lexer.h"
#include <cctype>
#include <unordered_map>

namespace binar {

static const std::unordered_map<std::string, TokenType> keywords = {
    {"fn",      TokenType::KW_FN},
    {"type",    TokenType::KW_TYPE},
    {"const",   TokenType::KW_CONST},
    {"import",  TokenType::KW_IMPORT},
    {"if",      TokenType::KW_IF},
    {"else",    TokenType::KW_ELSE},
    {"for",     TokenType::KW_FOR},
    {"while",   TokenType::KW_WHILE},
    {"return",  TokenType::KW_RETURN},
    {"break",   TokenType::KW_BREAK},
    {"continue", TokenType::KW_CONTINUE},
    {"range",   TokenType::KW_RANGE},
    {"defer",   TokenType::KW_DEFER},
    {"nil",     TokenType::KW_NIL},
    {"true",    TokenType::KW_TRUE},
    {"false",   TokenType::KW_FALSE},
    {"switch",  TokenType::KW_SWITCH},
    {"case",    TokenType::KW_CASE},
    {"default", TokenType::KW_DEFAULT},
    {"asm",     TokenType::KW_ASM},
    {"volatile",  TokenType::KW_VOLATILE},
    {"iface",     TokenType::KW_IFACE},
    {"from",      TokenType::KW_FROM},
    {"raise",     TokenType::KW_RAISE},
    {"int",     TokenType::TYPE_INT},
    {"float",   TokenType::TYPE_FLOAT},
    {"bool",    TokenType::TYPE_BOOL},
    {"string",  TokenType::TYPE_STRING},
    {"char",    TokenType::TYPE_CHAR},
    {"u8",      TokenType::TYPE_U8},
    {"u16",     TokenType::TYPE_U16},
    {"u32",     TokenType::TYPE_U32},
    {"u64",     TokenType::TYPE_U64},
    {"i8",      TokenType::TYPE_I8},
    {"i16",     TokenType::TYPE_I16},
    {"i32",     TokenType::TYPE_I32},
    {"i64",     TokenType::TYPE_I64},
    {"error",   TokenType::TYPE_ERROR},
};

const char* token_type_name(TokenType type) {
    switch (type) {
        case TokenType::INT_LIT: return "INT_LIT";
        case TokenType::FLOAT_LIT: return "FLOAT_LIT";
        case TokenType::STRING_LIT: return "STRING_LIT";
        case TokenType::CHAR_LIT: return "CHAR_LIT";
        case TokenType::BOOL_LIT: return "BOOL_LIT";
        case TokenType::IDENT: return "IDENT";
        case TokenType::KW_FN: return "fn";
        case TokenType::KW_TYPE: return "type";
        case TokenType::KW_CONST: return "const";
        case TokenType::KW_IMPORT: return "import";
        case TokenType::KW_IF: return "if";
        case TokenType::KW_ELSE: return "else";
        case TokenType::KW_FOR: return "for";
        case TokenType::KW_WHILE: return "while";
        case TokenType::KW_RETURN: return "return";
        case TokenType::KW_BREAK: return "break";
        case TokenType::KW_CONTINUE: return "continue";
        case TokenType::KW_RANGE: return "range";
        case TokenType::KW_DEFER: return "defer";
        case TokenType::KW_NIL: return "nil";
        case TokenType::KW_TRUE: return "true";
        case TokenType::KW_FALSE: return "false";
        case TokenType::KW_SWITCH: return "switch";
        case TokenType::KW_CASE: return "case";
        case TokenType::KW_DEFAULT: return "default";
        case TokenType::KW_ASM: return "asm";
        case TokenType::KW_VOLATILE: return "volatile";
        case TokenType::KW_IFACE: return "iface";
        case TokenType::KW_FROM: return "from";
        case TokenType::KW_RAISE: return "raise";
        case TokenType::TYPE_INT: return "int";
        case TokenType::TYPE_FLOAT: return "float";
        case TokenType::TYPE_BOOL: return "bool";
        case TokenType::TYPE_STRING: return "string";
        case TokenType::TYPE_CHAR: return "char";
        case TokenType::TYPE_U8: return "u8";
        case TokenType::TYPE_U16: return "u16";
        case TokenType::TYPE_U32: return "u32";
        case TokenType::TYPE_U64: return "u64";
        case TokenType::TYPE_I8: return "i8";
        case TokenType::TYPE_I16: return "i16";
        case TokenType::TYPE_I32: return "i32";
        case TokenType::TYPE_I64: return "i64";
        case TokenType::TYPE_ERROR: return "error";
        case TokenType::PLUS: return "+";
        case TokenType::MINUS: return "-";
        case TokenType::STAR: return "*";
        case TokenType::SLASH: return "/";
        case TokenType::PERCENT: return "%";
        case TokenType::AMP: return "&";
        case TokenType::PIPE: return "|";
        case TokenType::CARET: return "^";
        case TokenType::TILDE: return "~";
        case TokenType::LSHIFT: return "<<";
        case TokenType::RSHIFT: return ">>";
        case TokenType::ASSIGN: return "=";
        case TokenType::EQ: return "==";
        case TokenType::NEQ: return "!=";
        case TokenType::LT: return "<";
        case TokenType::GT: return ">";
        case TokenType::LTE: return "<=";
        case TokenType::GTE: return ">=";
        case TokenType::AND: return "&&";
        case TokenType::OR: return "||";
        case TokenType::NOT: return "!";
        case TokenType::PLUS_ASSIGN: return "+=";
        case TokenType::MINUS_ASSIGN: return "-=";
        case TokenType::STAR_ASSIGN: return "*=";
        case TokenType::SLASH_ASSIGN: return "/=";
        case TokenType::PERCENT_ASSIGN: return "%=";
        case TokenType::AMP_ASSIGN: return "&=";
        case TokenType::PIPE_ASSIGN: return "|=";
        case TokenType::CARET_ASSIGN: return "^=";
        case TokenType::LSHIFT_ASSIGN: return "<<=";
        case TokenType::RSHIFT_ASSIGN: return ">>=";
        case TokenType::PLUS_PLUS: return "++";
        case TokenType::MINUS_MINUS: return "--";
        case TokenType::LPAREN: return "(";
        case TokenType::RPAREN: return ")";
        case TokenType::LBRACE: return "{";
        case TokenType::RBRACE: return "}";
        case TokenType::LBRACKET: return "[";
        case TokenType::RBRACKET: return "]";
        case TokenType::COLON: return ":";
        case TokenType::COLON_ASSIGN: return ":=";
        case TokenType::SEMICOLON: return ";";
        case TokenType::COMMA: return ",";
        case TokenType::DOT: return ".";
        case TokenType::ARROW: return "->";
        case TokenType::NEWLINE: return "NEWLINE";
        case TokenType::EOF_TOKEN: return "EOF";
        case TokenType::INVALID: return "INVALID";
        default: return "UNKNOWN";
    }
}

Lexer::Lexer(const std::string& source, const std::string& filename)
    : source_(source), filename_(filename), pos_(0), line_(1), column_(1) {}

char Lexer::peek() const {
    if (pos_ >= source_.size()) return '\0';
    return source_[pos_];
}

char Lexer::peek_next() const {
    if (pos_ + 1 >= source_.size()) return '\0';
    return source_[pos_ + 1];
}

char Lexer::advance() {
    char c = source_[pos_++];
    if (c == '\n') {
        line_++;
        column_ = 1;
    } else {
        column_++;
    }
    return c;
}

bool Lexer::match(char expected) {
    if (pos_ >= source_.size() || source_[pos_] != expected) return false;
    advance();
    return true;
}

void Lexer::skip_whitespace() {
    while (pos_ < source_.size()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r') {
            advance();
        } else {
            break;
        }
    }
}

void Lexer::skip_comment() {
    // Block comment: // ...
    advance(); // skip first /
    while (pos_ < source_.size() && peek() != '\n') {
        advance();
    }
}

void Lexer::skip_line_comment() {
    while (pos_ < source_.size() && peek() != '\n') {
        advance();
    }
}

Token Lexer::read_string() {
    int start_line = line_;
    int start_col = column_;
    advance(); // skip opening "
    std::string value;
    while (pos_ < source_.size() && peek() != '"') {
        if (peek() == '\\') {
            advance();
            char c = advance();
            switch (c) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case '\\': value += '\\'; break;
                case '"': value += '"'; break;
                case '0': value += '\0'; break;
                default: value += c; break;
            }
        } else {
            value += advance();
        }
    }
    if (pos_ >= source_.size()) {
        return Token(TokenType::INVALID, "unterminated string", start_line, start_col);
    }
    advance(); // skip closing "
    return Token(TokenType::STRING_LIT, value, start_line, start_col);
}

Token Lexer::read_char() {
    int start_line = line_;
    int start_col = column_;
    advance(); // skip opening '
    std::string value;
    if (peek() == '\\') {
        advance();
        char c = advance();
        switch (c) {
            case 'n': value = "\n"; break;
            case 't': value = "\t"; break;
            case '\\': value = "\\"; break;
            case '\'': value = "'"; break;
            case '0': value = "\0"; break;
            default: value = std::string(1, c); break;
        }
    } else {
        value = std::string(1, advance());
    }
    if (peek() != '\'') {
        return Token(TokenType::INVALID, "unterminated char literal", start_line, start_col);
    }
    advance(); // skip closing '
    return Token(TokenType::CHAR_LIT, value, start_line, start_col);
}

Token Lexer::read_number() {
    int start_line = line_;
    int start_col = column_;
    std::string value;
    bool is_float = false;

    // Check for binary: 0b...
    if (peek() == '0' && peek_next() == 'b') {
        value += advance(); // '0'
        value += advance(); // 'b'
        while (pos_ < source_.size() && (peek() == '0' || peek() == '1' || peek() == '_')) {
            if (peek() != '_') value += advance(); else advance();
        }
        return Token(TokenType::INT_LIT, value, start_line, start_col);
    }

    // Check for hex: 0x...
    if (peek() == '0' && peek_next() == 'x') {
        value += advance(); // '0'
        value += advance(); // 'x'
        while (pos_ < source_.size() && std::isxdigit(peek())) {
            value += advance();
        }
        return Token(TokenType::INT_LIT, value, start_line, start_col);
    }

    while (pos_ < source_.size() && (std::isdigit(peek()) || peek() == '_')) {
        if (peek() != '_') value += advance(); else advance();
    }
    if (pos_ < source_.size() && peek() == '.' && peek_next() != '.') {
        is_float = true;
        value += advance(); // '.'
        while (pos_ < source_.size() && (std::isdigit(peek()) || peek() == '_')) {
            if (peek() != '_') value += advance(); else advance();
        }
    }

    return Token(is_float ? TokenType::FLOAT_LIT : TokenType::INT_LIT, value, start_line, start_col);
}

Token Lexer::read_identifier() {
    int start_line = line_;
    int start_col = column_;
    std::string value;
    while (pos_ < source_.size() && (std::isalnum(peek()) || peek() == '_')) {
        value += advance();
    }

    auto it = keywords.find(value);
    if (it != keywords.end()) {
        return Token(it->second, value, start_line, start_col);
    }
    return Token(TokenType::IDENT, value, start_line, start_col);
}

Token Lexer::make_token(TokenType type, const std::string& value) {
    return Token(type, value, line_, column_);
}

Token Lexer::error_token(const std::string& message) {
    return Token(TokenType::INVALID, message, line_, column_);
}

Token Lexer::next_token() {
    skip_whitespace();

    if (pos_ >= source_.size()) {
        return Token(TokenType::EOF_TOKEN, "", line_, column_);
    }

    // Emit NEWLINE tokens for line endings (implicit statement separators)
    if (peek() == '\n') {
        int nl_line = line_;
        int nl_col = column_;
        advance(); // consume \n
        skip_whitespace(); // skip indentation
        // Skip blank lines
        while (pos_ < source_.size() && peek() == '\n') {
            advance();
            skip_whitespace();
        }
        return Token(TokenType::NEWLINE, "\\n", nl_line, nl_col);
    }

    int start_line = line_;
    int start_col = column_;
    char c = peek();

    // Comments
    if (c == '/' && peek_next() == '/') {
        skip_line_comment();
        // After a comment, if we hit a newline, emit it
        if (pos_ < source_.size() && peek() == '\n') {
            int nl_line = line_;
            int nl_col = column_;
            advance();
            skip_whitespace();
            while (pos_ < source_.size() && peek() == '\n') {
                advance();
                skip_whitespace();
            }
            return Token(TokenType::NEWLINE, "\\n", nl_line, nl_col);
        }
        return next_token();
    }

    // String literal
    if (c == '"') return read_string();

    // Char literal
    if (c == '\'') return read_char();

    // Number literal
    if (std::isdigit(c)) return read_number();

    // Identifier or keyword
    if (std::isalpha(c) || c == '_') return read_identifier();

    advance();

    switch (c) {
        case '+':
            if (match('+')) return Token(TokenType::PLUS_PLUS, "++", start_line, start_col);
            if (match('=')) return Token(TokenType::PLUS_ASSIGN, "+=", start_line, start_col);
            return Token(TokenType::PLUS, "+", start_line, start_col);
        case '-':
            if (match('-')) return Token(TokenType::MINUS_MINUS, "--", start_line, start_col);
            if (match('=')) return Token(TokenType::MINUS_ASSIGN, "-=", start_line, start_col);
            if (match('>')) return Token(TokenType::ARROW, "->", start_line, start_col);
            return Token(TokenType::MINUS, "-", start_line, start_col);
        case '*':
            if (match('=')) return Token(TokenType::STAR_ASSIGN, "*=", start_line, start_col);
            return Token(TokenType::STAR, "*", start_line, start_col);
        case '/':
            if (match('=')) return Token(TokenType::SLASH_ASSIGN, "/=", start_line, start_col);
            return Token(TokenType::SLASH, "/", start_line, start_col);
        case '%':
            if (match('=')) return Token(TokenType::PERCENT_ASSIGN, "%=", start_line, start_col);
            return Token(TokenType::PERCENT, "%", start_line, start_col);
        case '&':
            if (match('&')) return Token(TokenType::AND, "&&", start_line, start_col);
            if (match('=')) return Token(TokenType::AMP_ASSIGN, "&=", start_line, start_col);
            return Token(TokenType::AMP, "&", start_line, start_col);
        case '|':
            if (match('|')) return Token(TokenType::OR, "||", start_line, start_col);
            if (match('=')) return Token(TokenType::PIPE_ASSIGN, "|=", start_line, start_col);
            return Token(TokenType::PIPE, "|", start_line, start_col);
        case '^':
            if (match('=')) return Token(TokenType::CARET_ASSIGN, "^=", start_line, start_col);
            return Token(TokenType::CARET, "^", start_line, start_col);
        case '~':
            return Token(TokenType::TILDE, "~", start_line, start_col);
        case '<':
            if (match('<')) {
                if (match('=')) return Token(TokenType::LSHIFT_ASSIGN, "<<=", start_line, start_col);
                return Token(TokenType::LSHIFT, "<<", start_line, start_col);
            }
            if (match('=')) return Token(TokenType::LTE, "<=", start_line, start_col);
            return Token(TokenType::LT, "<", start_line, start_col);
        case '>':
            if (match('>')) {
                if (match('=')) return Token(TokenType::RSHIFT_ASSIGN, ">>=", start_line, start_col);
                return Token(TokenType::RSHIFT, ">>", start_line, start_col);
            }
            if (match('=')) return Token(TokenType::GTE, ">=", start_line, start_col);
            return Token(TokenType::GT, ">", start_line, start_col);
        case '=':
            if (match('=')) return Token(TokenType::EQ, "==", start_line, start_col);
            return Token(TokenType::ASSIGN, "=", start_line, start_col);
        case '!':
            if (match('=')) return Token(TokenType::NEQ, "!=", start_line, start_col);
            return Token(TokenType::NOT, "!", start_line, start_col);
        case '(': return Token(TokenType::LPAREN, "(", start_line, start_col);
        case ')': return Token(TokenType::RPAREN, ")", start_line, start_col);
        case '{': return Token(TokenType::LBRACE, "{", start_line, start_col);
        case '}': return Token(TokenType::RBRACE, "}", start_line, start_col);
        case '[': return Token(TokenType::LBRACKET, "[", start_line, start_col);
        case ']': return Token(TokenType::RBRACKET, "]", start_line, start_col);
        case ':':
            if (match('=')) return Token(TokenType::COLON_ASSIGN, ":=", start_line, start_col);
            return Token(TokenType::COLON, ":", start_line, start_col);
        case ';': return Token(TokenType::SEMICOLON, ";", start_line, start_col);
        case ',': return Token(TokenType::COMMA, ",", start_line, start_col);
        case '.': return Token(TokenType::DOT, ".", start_line, start_col);
        default:
            return Token(TokenType::INVALID, std::string(1, c), start_line, start_col);
    }
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (true) {
        Token token = next_token();
        tokens.push_back(token);
        if (token.type == TokenType::EOF_TOKEN) break;
    }
    return tokens;
}

} // namespace binar
