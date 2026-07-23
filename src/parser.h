#pragma once

#include "token.h"
#include "ast.h"
#include <vector>
#include <string>
#include <stdexcept>
#include <set>

namespace binar {

class ParseError : public std::runtime_error {
public:
    int line;
    int column;
    ParseError(const std::string& msg, int line, int col)
        : std::runtime_error(msg), line(line), column(col) {}
};

class Parser {
public:
    Parser(const std::vector<Token>& tokens, const std::string& filename);

    Program parse();

private:
    // Helpers
    Token peek();
    Token peek_next();
    Token advance();
    bool check(TokenType type);
    bool check_next(TokenType type);
    bool match(TokenType type);
    bool match_any(std::initializer_list<TokenType> types);
    Token expect(TokenType type, const std::string& msg);
    [[noreturn]] void error(const std::string& msg);
    void skip_newlines();

    // Top-level
    Decl parse_decl();
    Decl parse_fn_decl();
    Decl parse_type_decl();
    Decl parse_iface_decl();
    Decl parse_const_decl();
    Decl parse_import();
    Decl parse_alias_decl();

    // Types
    TypePtr parse_type();
    TypePtr parse_named_type();
    TypePtr parse_pointer_type();
    TypePtr parse_slice_type();
    TypePtr parse_array_type();
    TypePtr parse_function_type();

    // Statements
    StmtPtr parse_stmt();
    StmtPtr parse_return_stmt();
    StmtPtr parse_if_stmt();
    StmtPtr parse_for_stmt();
    StmtPtr parse_while_stmt();
    StmtPtr parse_for_range_stmt();
    StmtPtr parse_defer_stmt();
    StmtPtr parse_asm_stmt();
    StmtPtr parse_switch_stmt();
    StmtPtr parse_block();
    StmtPtr parse_var_decl_or_expr_stmt();

    // Expressions
    ExprPtr parse_expr();
    ExprPtr parse_assign();
    ExprPtr parse_or();
    ExprPtr parse_and();
    ExprPtr parse_bit_or();
    ExprPtr parse_bit_xor();
    ExprPtr parse_bit_and();
    ExprPtr parse_equality();
    ExprPtr parse_comparison();
    ExprPtr parse_shift();
    ExprPtr parse_additive();
    ExprPtr parse_multiplicative();
    ExprPtr parse_unary();
    ExprPtr parse_postfix();
    ExprPtr parse_primary();

    ExprPtr parse_call_args(ExprPtr callee);

    std::vector<Token> tokens_;
    std::string filename_;
    size_t pos_;
    std::set<std::string> current_fn_type_params_;
};

} // namespace binar
