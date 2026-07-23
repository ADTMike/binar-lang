#pragma once

#include "token.h"
#include <string>
#include <vector>

namespace binar {

class Lexer {
public:
    Lexer(const std::string& source, const std::string& filename);

    std::vector<Token> tokenize();

private:
    char peek() const;
    char peek_next() const;
    char advance();
    bool match(char expected);

    void skip_whitespace();
    void skip_comment();
    void skip_line_comment();

    Token read_string();
    Token read_char();
    Token read_number();
    Token read_identifier();

    Token make_token(TokenType type, const std::string& value);
    Token error_token(const std::string& message);

    Token next_token();

    std::string source_;
    std::string filename_;
    size_t pos_;
    int line_;
    int column_;
};

} // namespace binar
