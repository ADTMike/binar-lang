#include "parser.h"
#include <sstream>

namespace binar {

Parser::Parser(const std::vector<Token>& tokens, const std::string& filename)
    : tokens_(tokens), filename_(filename), pos_(0) {}

Token Parser::peek() {
    if (pos_ >= tokens_.size()) {
        return Token(TokenType::EOF_TOKEN, "", 0, 0);
    }
    return tokens_[pos_];
}

Token Parser::peek_next() {
    if (pos_ + 1 >= tokens_.size()) {
        return Token(TokenType::EOF_TOKEN, "", 0, 0);
    }
    return tokens_[pos_ + 1];
}

Token Parser::advance() {
    Token t = tokens_[pos_];
    if (pos_ < tokens_.size()) pos_++;
    return t;
}

bool Parser::check(TokenType type) {
    return peek().type == type;
}

bool Parser::check_next(TokenType type) {
    return peek_next().type == type;
}

bool Parser::match(TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::match_any(std::initializer_list<TokenType> types) {
    for (auto type : types) {
        if (check(type)) {
            advance();
            return true;
        }
    }
    return false;
}

Token Parser::expect(TokenType type, const std::string& msg) {
    if (check(type)) return advance();
    error(msg);
}

void Parser::error(const std::string& msg) {
    Token t = peek();
    std::ostringstream oss;
    oss << filename_ << ":" << t.line << ":" << t.column << ": error: " << msg;
    if (!t.value.empty()) {
        oss << " near '" << t.value << "'";
    }
    throw ParseError(oss.str(), t.line, t.column);
}

void Parser::skip_newlines() {
    while (match(TokenType::NEWLINE)) {}
}

// ==================== Top-level ====================

Program Parser::parse() {
    Program program;
    while (true) {
        skip_newlines();
        if (check(TokenType::EOF_TOKEN)) break;
        program.decls.push_back(parse_decl());
    }
    return program;
}

Decl Parser::parse_decl() {
    if (check(TokenType::KW_FN)) return parse_fn_decl();
    if (check(TokenType::KW_TYPE)) return parse_type_decl();
    if (check(TokenType::KW_IFACE)) return parse_iface_decl();
    if (check(TokenType::KW_CONST)) return parse_const_decl();
    if (check(TokenType::KW_IMPORT)) return parse_import();
    error("unexpected token at top level");
}

Decl Parser::parse_fn_decl() {
    Decl decl;
    decl.kind = DeclKind::FN;
    Token fn_tok = advance(); // consume 'fn'
    decl.line = fn_tok.line;
    decl.column = fn_tok.column;

    decl.fn.name = expect(TokenType::IDENT, "expected function name").value;

    // Type parameters: [T], [T Logger], [T, U]
    current_fn_type_params_.clear();
    if (match(TokenType::LBRACKET)) {
        while (!check(TokenType::RBRACKET) && !check(TokenType::EOF_TOKEN)) {
            skip_newlines();
            std::string tp_name = expect(TokenType::IDENT, "expected type parameter").value;
            current_fn_type_params_.insert(tp_name);
            decl.fn.type_params.push_back(tp_name);

            // Optional constraint: T Logger
            if (check(TokenType::IDENT) && peek().value != ",") {
                TypePtr constraint = parse_type();
                decl.fn.type_constraints[tp_name] = std::move(constraint);
            }

            if (!match(TokenType::COMMA)) break;
        }
        expect(TokenType::RBRACKET, "expected ']'");
    }

    // Parameters: (p *Type, x int, ...)
    expect(TokenType::LPAREN, "expected '('");
    while (!check(TokenType::RPAREN) && !check(TokenType::EOF_TOKEN)) {
        FnParam param;
        param.name = expect(TokenType::IDENT, "expected parameter name").value;
        param.type = parse_type();
        decl.fn.params.push_back(std::move(param));
        if (!check(TokenType::RPAREN)) {
            expect(TokenType::COMMA, "expected ',' or ')'");
        }
    }
    expect(TokenType::RPAREN, "expected ')'");

    // Return types: (int, int) or int or nothing
    if (check(TokenType::LPAREN)) {
        // Multiple returns without arrow: (int, int)
        advance(); // consume '('
        while (!check(TokenType::RPAREN) && !check(TokenType::EOF_TOKEN)) {
            decl.fn.return_types.push_back(parse_type());
            if (!check(TokenType::RPAREN)) {
                expect(TokenType::COMMA, "expected ',' or ')'");
            }
        }
        expect(TokenType::RPAREN, "expected ')'");
    } else if (!check(TokenType::LBRACE)) {
        // Single return type without arrow: fn Add(a int, b int) int
        if (check(TokenType::TYPE_INT) || check(TokenType::TYPE_FLOAT) ||
            check(TokenType::TYPE_BOOL) || check(TokenType::TYPE_STRING) ||
            check(TokenType::TYPE_CHAR) || check(TokenType::TYPE_ERROR) ||
            check(TokenType::TYPE_U8) || check(TokenType::TYPE_U16) ||
            check(TokenType::TYPE_U32) || check(TokenType::TYPE_U64) ||
            check(TokenType::TYPE_I8) || check(TokenType::TYPE_I16) ||
            check(TokenType::TYPE_I32) || check(TokenType::TYPE_I64) ||
            check(TokenType::STAR) ||
            check(TokenType::IDENT)) {
            decl.fn.return_types.push_back(parse_type());
        }
    }

    // Body
    if (check(TokenType::LBRACE)) {
        Token body_start = peek();
        advance(); // consume '{'
        while (true) {
            skip_newlines();
            if (check(TokenType::RBRACE) || check(TokenType::EOF_TOKEN)) break;
            decl.fn.body.push_back(parse_stmt());
        }
        expect(TokenType::RBRACE, "expected '}'");
        decl.fn.has_body = true;
    } else {
        decl.fn.has_body = false;
    }

    return decl;
}

Decl Parser::parse_type_decl() {
    Decl decl;
    decl.kind = DeclKind::TYPE;
    Token type_tok = advance(); // consume 'type'
    decl.line = type_tok.line;
    decl.column = type_tok.column;

    decl.type_decl.name = expect(TokenType::IDENT, "expected type name").value;
    expect(TokenType::LBRACE, "expected '{'");

    while (true) {
        skip_newlines();
        if (check(TokenType::RBRACE) || check(TokenType::EOF_TOKEN)) break;
        FieldDecl field;
        field.name = expect(TokenType::IDENT, "expected field name").value;
        field.type = parse_type();
            decl.type_decl.fields.push_back(std::move(field));
    }
    expect(TokenType::RBRACE, "expected '}'");
    return decl;
}

Decl Parser::parse_iface_decl() {
    Decl decl;
    decl.kind = DeclKind::IFACE;
    Token iface_tok = advance(); // consume 'iface'
    decl.line = iface_tok.line;
    decl.column = iface_tok.column;

    decl.iface_decl.name = expect(TokenType::IDENT, "expected interface name").value;
    expect(TokenType::LBRACE, "expected '{'");

    while (true) {
        skip_newlines();
        if (check(TokenType::RBRACE) || check(TokenType::EOF_TOKEN)) break;

        expect(TokenType::KW_FN, "expected 'fn'");
        std::string method_name = expect(TokenType::IDENT, "expected method name").value;

        expect(TokenType::LPAREN, "expected '('");
        bool is_pointer = check(TokenType::STAR);
        TypePtr param_type = parse_type();
        expect(TokenType::RPAREN, "expected ')'");

        InterfaceMethod method;
        method.name = method_name;
        method.param_type = std::move(param_type);
        method.is_pointer = is_pointer;
        decl.iface_decl.methods.push_back(std::move(method));

        skip_newlines();
    }

    expect(TokenType::RBRACE, "expected '}'");
    return decl;
}

Decl Parser::parse_const_decl() {
    Decl decl;
    decl.kind = DeclKind::CONST;
    Token const_tok = advance(); // consume 'const'
    decl.line = const_tok.line;
    decl.column = const_tok.column;

    decl.const_decl.name = expect(TokenType::IDENT, "expected constant name").value;

    // Optional type annotation
    if (!check(TokenType::ASSIGN)) {
        decl.const_decl.type = parse_type();
    }

    expect(TokenType::ASSIGN, "expected '='");
    decl.const_decl.value = parse_expr();
    return decl;
}

Decl Parser::parse_import() {
    Decl decl;
    decl.kind = DeclKind::IMPORT;
    Token import_tok = advance(); // consume 'import'
    decl.line = import_tok.line;
    decl.column = import_tok.column;

    expect(TokenType::LBRACE, "expected '{' after import");
    skip_newlines();

    while (true) {
        if (check(TokenType::RBRACE) || check(TokenType::EOF_TOKEN)) break;
        ImportBinding binding;
        binding.name = expect(TokenType::IDENT, "expected import name").value;
        decl.import_block.bindings.push_back(binding);
        skip_newlines();
        if (check(TokenType::COMMA)) {
            advance();
            skip_newlines();
        } else {
            break;
        }
    }
    expect(TokenType::RBRACE, "expected '}'");

    decl.import_block.is_from = false;
    skip_newlines();
    if (match(TokenType::KW_FROM)) {
        decl.import_block.is_from = true;
        skip_newlines();
        std::string pkg = expect(TokenType::STRING_LIT, "expected package path").value;
        for (auto& b : decl.import_block.bindings) {
            b.package_path = pkg;
        }
    }

    return decl;
}

// ==================== Types ====================

TypePtr Parser::parse_type() {
    // Pointer type
    if (match(TokenType::STAR)) {
        auto type = std::make_unique<TypeAnnotation>();
        type->kind = TypeKind::POINTER;
        type->inner = parse_type();
        return type;
    }

    // Slice type: []Type
    if (check(TokenType::LBRACKET) && check_next(TokenType::RBRACKET)) {
        advance(); // consume '['
        advance(); // consume ']'
        auto type = std::make_unique<TypeAnnotation>();
        type->kind = TypeKind::SLICE;
        type->inner = parse_type();
        return type;
    }

    // Array type: [N]Type
    if (check(TokenType::LBRACKET) && check_next(TokenType::INT_LIT)) {
        advance(); // consume '['
        Token size = advance(); // consume size
        expect(TokenType::RBRACKET, "expected ']'");
        auto type = std::make_unique<TypeAnnotation>();
        type->kind = TypeKind::ARRAY;
        type->array_size = std::stoi(size.value);
        type->inner = parse_type();
        return type;
    }

    // Array type with const: [CONST_NAME]Type
    if (check(TokenType::LBRACKET) && check_next(TokenType::IDENT)) {
        advance(); // consume '['
        Token size_name = advance(); // consume const name
        expect(TokenType::RBRACKET, "expected ']'");
        auto type = std::make_unique<TypeAnnotation>();
        type->kind = TypeKind::ARRAY;
        type->array_size_name = size_name.value;
        type->inner = parse_type();
        return type;
    }

    // Function type: fn(ParamTypes) -> RetTypes
    if (check(TokenType::KW_FN)) {
        advance(); // consume 'fn'
        auto type = std::make_unique<TypeAnnotation>();
        type->kind = TypeKind::FUNCTION;

        expect(TokenType::LPAREN, "expected '('");
        while (!check(TokenType::RPAREN) && !check(TokenType::EOF_TOKEN)) {
            type->fn_params.push_back({"", parse_type()});
            if (!check(TokenType::RPAREN)) {
                expect(TokenType::COMMA, "expected ',' or ')'");
            }
        }
        expect(TokenType::RPAREN, "expected ')'");

        return type;
    }

    // Named type
    return parse_named_type();
}

TypePtr Parser::parse_named_type() {
    // Accept both IDENT (user types) and TYPE_* tokens (built-in types)
    Token name;
    if (check(TokenType::IDENT)) {
        name = advance();
    } else if (check(TokenType::TYPE_INT) || check(TokenType::TYPE_FLOAT) ||
               check(TokenType::TYPE_BOOL) || check(TokenType::TYPE_STRING) ||
               check(TokenType::TYPE_CHAR) || check(TokenType::TYPE_ERROR) ||
               check(TokenType::TYPE_U8) || check(TokenType::TYPE_U16) ||
               check(TokenType::TYPE_U32) || check(TokenType::TYPE_U64) ||
               check(TokenType::TYPE_I8) || check(TokenType::TYPE_I16) ||
               check(TokenType::TYPE_I32) || check(TokenType::TYPE_I64)) {
        name = advance();
    } else {
        error("expected type name");
    }

    auto type = std::make_unique<TypeAnnotation>();

    // Check if this is a type parameter
    if (current_fn_type_params_.count(name.value)) {
        type->kind = TypeKind::TYPE_PARAM;
        type->name = name.value;
    } else {
        type->kind = TypeKind::NAMED;
        type->name = name.value;
    }
    return type;
}

// ==================== Statements ====================

StmtPtr Parser::parse_stmt() {
    if (check(TokenType::KW_RETURN)) return parse_return_stmt();
    if (check(TokenType::KW_IF)) return parse_if_stmt();
    if (check(TokenType::KW_FOR)) return parse_for_stmt();
    if (check(TokenType::KW_WHILE)) return parse_while_stmt();
    if (check(TokenType::KW_DEFER)) return parse_defer_stmt();
    if (check(TokenType::KW_SWITCH)) return parse_switch_stmt();
    if (check(TokenType::KW_ASM)) return parse_asm_stmt();
    if (check(TokenType::KW_BREAK)) {
        Token t = advance();
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::BREAK;
        stmt->line = t.line;
        stmt->column = t.column;
        return stmt;
    }
    if (check(TokenType::KW_CONTINUE)) {
        Token t = advance();
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::CONTINUE;
        stmt->line = t.line;
        stmt->column = t.column;
        return stmt;
    }
    if (check(TokenType::LBRACE)) return parse_block();
    return parse_var_decl_or_expr_stmt();
}

StmtPtr Parser::parse_return_stmt() {
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = StmtKind::RETURN;
    Token ret = advance(); // consume 'return'
    stmt->line = ret.line;
    stmt->column = ret.column;

    if (!check(TokenType::RBRACE) && !check(TokenType::EOF_TOKEN)) {
        stmt->return_values.push_back(parse_expr());
        while (match(TokenType::COMMA)) {
            stmt->return_values.push_back(parse_expr());
        }
        // Keep expr for single return for backward compatibility
        if (stmt->return_values.size() == 1) {
            stmt->expr = std::move(stmt->return_values[0]);
            stmt->return_values.clear();
        }
    }
    return stmt;
}

StmtPtr Parser::parse_if_stmt() {
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = StmtKind::IF;
    Token if_tok = advance(); // consume 'if'
    stmt->line = if_tok.line;
    stmt->column = if_tok.column;

    // if condition { ... } else if condition { ... } else { ... }
    while (true) {
        IfBranch branch;
        branch.condition = parse_expr();
        expect(TokenType::LBRACE, "expected '{'");
        while (true) {
            skip_newlines();
            if (check(TokenType::RBRACE) || check(TokenType::EOF_TOKEN)) break;
            branch.body.push_back(parse_stmt());
        }
        expect(TokenType::RBRACE, "expected '}'");
        stmt->if_stmt.branches.push_back(std::move(branch));

        skip_newlines();
        if (match(TokenType::KW_ELSE)) {
            if (check(TokenType::KW_IF)) {
                advance(); // consume 'if'
                continue; // parse next branch
            } else {
                // else block
                expect(TokenType::LBRACE, "expected '{'");
                while (true) {
                    skip_newlines();
                    if (check(TokenType::RBRACE) || check(TokenType::EOF_TOKEN)) break;
                    stmt->if_stmt.else_body.push_back(parse_stmt());
                }
                expect(TokenType::RBRACE, "expected '}'");
                break; // else block is final branch
            }
        } else {
            break;
        }
    }
    return stmt;
}

StmtPtr Parser::parse_for_stmt() {
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = StmtKind::FOR;
    Token for_tok = advance(); // consume 'for'
    stmt->line = for_tok.line;
    stmt->column = for_tok.column;

    // Check if this is a for-range: for i, v := range expr or for _, v := range expr
    if (check(TokenType::IDENT) && check_next(TokenType::COMMA)) {
        return parse_for_range_stmt();
    }
    if (check(TokenType::IDENT)) {
        // Peek ahead: name , name := range
        size_t saved = pos_;
        advance(); // name
        if (match(TokenType::COMMA)) {
            if (check(TokenType::IDENT) || check(TokenType::IDENT)) {
                advance(); // value name
                if (match(TokenType::COLON) && match(TokenType::ASSIGN) && check(TokenType::KW_RANGE)) {
                    pos_ = saved;
                    return parse_for_range_stmt();
                }
            }
        }
        pos_ = saved;
    }

    // Standard for loop: for init; cond; update { body }
    if (!check(TokenType::SEMICOLON)) {
        stmt->for_stmt.init = parse_var_decl_or_expr_stmt();
    }
    expect(TokenType::SEMICOLON, "expected ';'");

    if (!check(TokenType::SEMICOLON)) {
        stmt->for_stmt.cond = parse_expr();
    }
    expect(TokenType::SEMICOLON, "expected ';'");

    if (!check(TokenType::LBRACE)) {
        stmt->for_stmt.update = parse_expr();
    }

    expect(TokenType::LBRACE, "expected '{'");
    while (true) {
        skip_newlines();
        if (check(TokenType::RBRACE) || check(TokenType::EOF_TOKEN)) break;
        stmt->for_stmt.body.push_back(parse_stmt());
    }
    expect(TokenType::RBRACE, "expected '}'");
    return stmt;
}

StmtPtr Parser::parse_while_stmt() {
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = StmtKind::WHILE;
    Token while_tok = advance(); // consume 'while'
    stmt->line = while_tok.line;
    stmt->column = while_tok.column;

    stmt->while_stmt.condition = parse_expr();
    expect(TokenType::LBRACE, "expected '{'");
    while (true) {
        skip_newlines();
        if (check(TokenType::RBRACE) || check(TokenType::EOF_TOKEN)) break;
        stmt->while_stmt.body.push_back(parse_stmt());
    }
    expect(TokenType::RBRACE, "expected '}'");
    return stmt;
}

StmtPtr Parser::parse_for_range_stmt() {
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = StmtKind::FOR_RANGE;
    Token for_tok = peek();
    stmt->line = for_tok.line;
    stmt->column = for_tok.column;

    // Parse: for [name,] name := range expr
    if (check(TokenType::IDENT) && peek().value == "_") {
        advance(); // consume '_'
        stmt->range_stmt.index_name = "_";
        expect(TokenType::COMMA, "expected ','");
    } else {
        stmt->range_stmt.index_name = expect(TokenType::IDENT, "expected index variable").value;
        if (match(TokenType::COMMA)) {
            if (check(TokenType::IDENT) && peek().value == "_") {
                advance();
                stmt->range_stmt.value_name = "_";
            } else {
                stmt->range_stmt.value_name = expect(TokenType::IDENT, "expected value variable").value;
            }
        }
    }

    expect(TokenType::COLON_ASSIGN, "expected ':='");
    expect(TokenType::KW_RANGE, "expected 'range'");
    stmt->range_stmt.range_expr = parse_expr();

    expect(TokenType::LBRACE, "expected '{'");
    while (true) {
        skip_newlines();
        if (check(TokenType::RBRACE) || check(TokenType::EOF_TOKEN)) break;
        stmt->range_stmt.body.push_back(parse_stmt());
    }
    expect(TokenType::RBRACE, "expected '}'");
    return stmt;
}

StmtPtr Parser::parse_defer_stmt() {
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = StmtKind::DEFER;
    Token defer_tok = advance(); // consume 'defer'
    stmt->line = defer_tok.line;
    stmt->column = defer_tok.column;
    stmt->defer_stmt.expr = parse_expr();
    return stmt;
}

StmtPtr Parser::parse_asm_stmt() {
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = StmtKind::ASM;
    Token asm_tok = advance(); // consume 'asm'
    stmt->line = asm_tok.line;
    stmt->column = asm_tok.column;
    stmt->asm_stmt.is_volatile = false;

    // Optional "volatile" keyword before the string
    if (check(TokenType::KW_VOLATILE)) {
        advance();
        stmt->asm_stmt.is_volatile = true;
    }

    // Optional '(' for Zig-style asm("code" : ...)
    bool has_parens = match(TokenType::LPAREN);

    stmt->asm_stmt.code = expect(TokenType::STRING_LIT, "expected inline assembly string").value;

    // Optional operands: asm("code" : outputs : inputs : clobbers)
    if (match(TokenType::COLON)) {
        // Parse output operands
        if (!check(TokenType::COLON) && !check(TokenType::RBRACE) && !check(TokenType::EOF_TOKEN)) {
            while (true) {
                AsmOperand op;
                op.is_output = true;
                // Optional name: [name]
                if (match(TokenType::LBRACKET)) {
                    op.name = expect(TokenType::IDENT, "expected operand name").value;
                    expect(TokenType::RBRACKET, "expected ']'");
                }
                op.constraint = expect(TokenType::STRING_LIT, "expected constraint string").value;
                if (match(TokenType::LPAREN)) {
                    op.value = parse_expr();
                    expect(TokenType::RPAREN, "expected ')'");
                }
                stmt->asm_stmt.outputs.push_back(std::move(op));
                if (!match(TokenType::COMMA)) break;
            }
        }

        if (match(TokenType::COLON)) {
            // Parse input operands
            if (!check(TokenType::COLON) && !check(TokenType::RBRACE) && !check(TokenType::EOF_TOKEN)) {
                while (true) {
                    AsmOperand op;
                    op.is_output = false;
                    // Optional name: [name]
                    if (match(TokenType::LBRACKET)) {
                        op.name = expect(TokenType::IDENT, "expected operand name").value;
                        expect(TokenType::RBRACKET, "expected ']'");
                    }
                    op.constraint = expect(TokenType::STRING_LIT, "expected constraint string").value;
                    if (match(TokenType::LPAREN)) {
                        op.value = parse_expr();
                        expect(TokenType::RPAREN, "expected ')'");
                    }
                    stmt->asm_stmt.inputs.push_back(std::move(op));
                    if (!match(TokenType::COMMA)) break;
                }
            }

            if (match(TokenType::COLON)) {
                // Parse clobbers
                while (true) {
                    stmt->asm_stmt.clobbers.push_back(
                        expect(TokenType::STRING_LIT, "expected clobber string").value);
                    if (!match(TokenType::COMMA)) break;
                }
            }
        }
    }

    if (has_parens) {
        expect(TokenType::RPAREN, "expected ')'");
    }

    return stmt;
}

StmtPtr Parser::parse_switch_stmt() {
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = StmtKind::SWITCH;
    Token switch_tok = advance(); // consume 'switch'
    stmt->line = switch_tok.line;
    stmt->column = switch_tok.column;

    // Optional switch value
    if (!check(TokenType::LBRACE)) {
        stmt->switch_stmt.value = parse_expr();
    }

    expect(TokenType::LBRACE, "expected '{'");

    while (true) {
        skip_newlines();
        if (check(TokenType::RBRACE) || check(TokenType::EOF_TOKEN)) break;
        SwitchCase sc;
        if (match(TokenType::KW_DEFAULT)) {
            sc.is_default = true;
            expect(TokenType::COLON, "expected ':'");
        } else {
            sc.is_default = false;
            expect(TokenType::KW_CASE, "expected 'case'");
            sc.values.push_back(parse_expr());
            while (match(TokenType::COMMA)) {
                sc.values.push_back(parse_expr());
            }
            expect(TokenType::COLON, "expected ':'");
        }

        while (true) {
            skip_newlines();
            if (check(TokenType::RBRACE) || check(TokenType::KW_CASE) ||
                check(TokenType::KW_DEFAULT) || check(TokenType::EOF_TOKEN)) break;
            sc.body.push_back(parse_stmt());
        }
        stmt->switch_stmt.cases.push_back(std::move(sc));
    }

    expect(TokenType::RBRACE, "expected '}'");
    return stmt;
}

StmtPtr Parser::parse_block() {
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = StmtKind::BLOCK;
    Token brace = advance(); // consume '{'
    stmt->line = brace.line;
    stmt->column = brace.column;

    while (true) {
        skip_newlines();
        if (check(TokenType::RBRACE) || check(TokenType::EOF_TOKEN)) break;
        stmt->block.stmts.push_back(parse_stmt());
    }
    expect(TokenType::RBRACE, "expected '}'");
    return stmt;
}

StmtPtr Parser::parse_var_decl_or_expr_stmt() {
    // Check for multi-target: name, name := expr or name, name = expr
    if (check(TokenType::IDENT) && check_next(TokenType::COMMA)) {
        // Look ahead to see if this is multi-target
        size_t saved = pos_;
        std::vector<std::string> names;
        while (check(TokenType::IDENT)) {
            names.push_back(advance().value);
            if (!match(TokenType::COMMA)) break;
        }

        if (names.size() > 1 && (check(TokenType::COLON_ASSIGN) || check(TokenType::ASSIGN))) {
            bool is_decl = check(TokenType::COLON_ASSIGN);
            advance(); // consume := or =

            auto stmt = std::make_unique<Stmt>();
            stmt->kind = StmtKind::MULTI_ASSIGN;
            stmt->line = tokens_[saved].line;
            stmt->column = tokens_[saved].column;
            stmt->multi_assign.targets = std::move(names);
            stmt->multi_assign.value = parse_expr();
            stmt->multi_assign.is_decl = is_decl;
            return stmt;
        }

        pos_ = saved;
    }

    // Check for := declaration: name := expr
    if (check(TokenType::IDENT) && check_next(TokenType::COLON_ASSIGN)) {
        Token name = advance(); // consume name
        advance(); // consume :=

        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::EXPR;
        stmt->line = name.line;
        stmt->column = name.column;

        auto assign_expr = std::make_unique<Expr>();
        assign_expr->kind = ExprKind::ASSIGN;
        assign_expr->line = name.line;
        assign_expr->column = name.column;

        auto target = std::make_unique<Expr>();
        target->kind = ExprKind::IDENT;
        target->ident = name.value;
        target->line = name.line;
        target->column = name.column;
        assign_expr->assign.target = std::move(target);
        assign_expr->assign.value = parse_expr();

        if (check(TokenType::KW_RAISE)) {
            advance();
            assign_expr->assign.raise = true;
        }

        assign_expr->assign.is_decl = true;
        assign_expr->assign.has_type = false;
        assign_expr->assign.assign_op = TokenType::ASSIGN;

        stmt->expr = std::move(assign_expr);
        return stmt;
    }

    // Check for explicit type declaration: name type [= expr]
    if (check(TokenType::IDENT)) {
        Token name = peek();
        size_t saved_pos = pos_;

        advance(); // consume name

        // Check if next token is a type (not :=, not ., not ()
        if (check(TokenType::TYPE_INT) || check(TokenType::TYPE_FLOAT) ||
            check(TokenType::TYPE_BOOL) || check(TokenType::TYPE_STRING) ||
            check(TokenType::TYPE_CHAR) || check(TokenType::TYPE_ERROR) ||
            (check(TokenType::IDENT) && !check_next(TokenType::LPAREN) && !check_next(TokenType::DOT))) {
            // Advance past the type token to check what follows
            advance(); // consume type
            // Check what follows the type
            if (check(TokenType::ASSIGN) || check(TokenType::SEMICOLON) ||
                check(TokenType::NEWLINE) || check(TokenType::EOF_TOKEN) ||
                check(TokenType::RBRACE) || check(TokenType::RPAREN) ||
                check(TokenType::COMMA) || check(TokenType::IDENT) ||
                check_next(TokenType::RBRACE) || check_next(TokenType::EOF_TOKEN)) {
                pos_ = saved_pos;
                advance(); // consume name again

                auto stmt = std::make_unique<Stmt>();
                stmt->kind = StmtKind::EXPR;
                stmt->line = name.line;
                stmt->column = name.column;

                auto assign_expr = std::make_unique<Expr>();
                assign_expr->kind = ExprKind::ASSIGN;
                assign_expr->line = name.line;
                assign_expr->column = name.column;

                auto target = std::make_unique<Expr>();
                target->kind = ExprKind::IDENT;
                target->ident = name.value;
                target->line = name.line;
                target->column = name.column;
                assign_expr->assign.target = std::move(target);

                // Consume and store the type annotation
                assign_expr->assign.has_type = true;
                assign_expr->assign.type_name = peek().value;
                advance(); // consume type token

                if (match(TokenType::ASSIGN)) {
                    assign_expr->assign.value = parse_expr();
                } else {
                    assign_expr->assign.value = nullptr;
                }
                assign_expr->assign.is_decl = true;
                assign_expr->assign.assign_op = TokenType::ASSIGN;

                stmt->expr = std::move(assign_expr);
                return stmt;
            }
        }

        pos_ = saved_pos;
    }

    // Expression statement
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = StmtKind::EXPR;
    stmt->expr = parse_expr();
    if (check(TokenType::KW_RAISE)) {
        advance();
        stmt->raise = true;
    }
    return stmt;
}

// ==================== Expressions ====================

ExprPtr Parser::parse_expr() {
    return parse_assign();
}

ExprPtr Parser::parse_assign() {
    auto left = parse_or();

    if (match(TokenType::ASSIGN) || match(TokenType::PLUS_ASSIGN) ||
        match(TokenType::MINUS_ASSIGN) || match(TokenType::STAR_ASSIGN) ||
        match(TokenType::SLASH_ASSIGN) || match(TokenType::PERCENT_ASSIGN) ||
        match(TokenType::AMP_ASSIGN) || match(TokenType::PIPE_ASSIGN) ||
        match(TokenType::CARET_ASSIGN) || match(TokenType::LSHIFT_ASSIGN) ||
        match(TokenType::RSHIFT_ASSIGN)) {
        Token op = tokens_[pos_ - 1];
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::ASSIGN;
        expr->line = op.line;
        expr->column = op.column;
        expr->assign.target = std::move(left);
        expr->assign.value = parse_assign();
        expr->assign.is_decl = false;
        expr->assign.has_type = false;
        expr->assign.assign_op = op.type;
        return expr;
    }

    return left;
}

ExprPtr Parser::parse_or() {
    auto left = parse_and();
    while (match(TokenType::OR)) {
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::BINARY;
        expr->binary.op = BinOp::OR;
        expr->binary.left = std::move(left);
        expr->binary.right = parse_and();
        expr->line = expr->binary.left->line;
        expr->column = expr->binary.left->column;
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::parse_and() {
    auto left = parse_bit_or();
    while (match(TokenType::AND)) {
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::BINARY;
        expr->binary.op = BinOp::AND;
        expr->binary.left = std::move(left);
        expr->binary.right = parse_bit_or();
        expr->line = expr->binary.left->line;
        expr->column = expr->binary.left->column;
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::parse_bit_or() {
    auto left = parse_bit_xor();
    while (match(TokenType::PIPE)) {
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::BINARY;
        expr->binary.op = BinOp::BIT_OR;
        expr->binary.left = std::move(left);
        expr->binary.right = parse_bit_xor();
        expr->line = expr->binary.left->line;
        expr->column = expr->binary.left->column;
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::parse_bit_xor() {
    auto left = parse_bit_and();
    while (match(TokenType::CARET)) {
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::BINARY;
        expr->binary.op = BinOp::BIT_XOR;
        expr->binary.left = std::move(left);
        expr->binary.right = parse_bit_and();
        expr->line = expr->binary.left->line;
        expr->column = expr->binary.left->column;
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::parse_bit_and() {
    auto left = parse_equality();
    while (match(TokenType::AMP)) {
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::BINARY;
        expr->binary.op = BinOp::BIT_AND;
        expr->binary.left = std::move(left);
        expr->binary.right = parse_equality();
        expr->line = expr->binary.left->line;
        expr->column = expr->binary.left->column;
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::parse_equality() {
    auto left = parse_comparison();
    while (match_any({TokenType::EQ, TokenType::NEQ})) {
        Token op = tokens_[pos_ - 1];
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::BINARY;
        expr->binary.op = (op.type == TokenType::EQ) ? BinOp::EQ : BinOp::NEQ;
        expr->binary.left = std::move(left);
        expr->binary.right = parse_comparison();
        expr->line = expr->binary.left->line;
        expr->column = expr->binary.left->column;
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::parse_comparison() {
    auto left = parse_shift();
    while (match_any({TokenType::LT, TokenType::GT, TokenType::LTE, TokenType::GTE})) {
        Token op = tokens_[pos_ - 1];
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::BINARY;
        switch (op.type) {
            case TokenType::LT: expr->binary.op = BinOp::LT; break;
            case TokenType::GT: expr->binary.op = BinOp::GT; break;
            case TokenType::LTE: expr->binary.op = BinOp::LTE; break;
            case TokenType::GTE: expr->binary.op = BinOp::GTE; break;
            default: break;
        }
        expr->binary.left = std::move(left);
        expr->binary.right = parse_shift();
        expr->line = expr->binary.left->line;
        expr->column = expr->binary.left->column;
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::parse_shift() {
    auto left = parse_additive();
    while (match_any({TokenType::LSHIFT, TokenType::RSHIFT})) {
        Token op = tokens_[pos_ - 1];
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::BINARY;
        expr->binary.op = (op.type == TokenType::LSHIFT) ? BinOp::LSHIFT : BinOp::RSHIFT;
        expr->binary.left = std::move(left);
        expr->binary.right = parse_additive();
        expr->line = expr->binary.left->line;
        expr->column = expr->binary.left->column;
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::parse_additive() {
    auto left = parse_multiplicative();
    while (match_any({TokenType::PLUS, TokenType::MINUS})) {
        Token op = tokens_[pos_ - 1];
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::BINARY;
        expr->binary.op = (op.type == TokenType::PLUS) ? BinOp::ADD : BinOp::SUB;
        expr->binary.left = std::move(left);
        expr->binary.right = parse_multiplicative();
        expr->line = expr->binary.left->line;
        expr->column = expr->binary.left->column;
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::parse_multiplicative() {
    auto left = parse_unary();
    while (match_any({TokenType::STAR, TokenType::SLASH, TokenType::PERCENT})) {
        Token op = tokens_[pos_ - 1];
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::BINARY;
        switch (op.type) {
            case TokenType::STAR: expr->binary.op = BinOp::MUL; break;
            case TokenType::SLASH: expr->binary.op = BinOp::DIV; break;
            case TokenType::PERCENT: expr->binary.op = BinOp::MOD; break;
            default: break;
        }
        expr->binary.left = std::move(left);
        expr->binary.right = parse_unary();
        expr->line = expr->binary.left->line;
        expr->column = expr->binary.left->column;
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::parse_unary() {
    if (match(TokenType::MINUS)) {
        Token op = tokens_[pos_ - 1];
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::UNARY;
        expr->unary.op = UnOp::NEG;
        expr->unary.operand = parse_unary();
        expr->line = op.line;
        expr->column = op.column;
        return expr;
    }
    if (match(TokenType::NOT)) {
        Token op = tokens_[pos_ - 1];
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::UNARY;
        expr->unary.op = UnOp::NOT;
        expr->unary.operand = parse_unary();
        expr->line = op.line;
        expr->column = op.column;
        return expr;
    }
    if (match(TokenType::TILDE)) {
        Token op = tokens_[pos_ - 1];
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::UNARY;
        expr->unary.op = UnOp::BIT_NOT;
        expr->unary.operand = parse_unary();
        expr->line = op.line;
        expr->column = op.column;
        return expr;
    }
    if (match(TokenType::AMP)) {
        Token op = tokens_[pos_ - 1];
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::UNARY;
        expr->unary.op = UnOp::ADDR_OF;
        expr->unary.operand = parse_unary();
        expr->line = op.line;
        expr->column = op.column;
        return expr;
    }
    if (match(TokenType::STAR)) {
        Token op = tokens_[pos_ - 1];
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::UNARY;
        expr->unary.op = UnOp::DEREF;
        expr->unary.operand = parse_unary();
        expr->line = op.line;
        expr->column = op.column;
        return expr;
    }
    return parse_postfix();
}

ExprPtr Parser::parse_postfix() {
    auto expr = parse_primary();

    while (true) {
        // Function call: expr(args)
        if (check(TokenType::LPAREN)) {
            expr = parse_call_args(std::move(expr));
            continue;
        }

        // Index: expr[index]
        if (check(TokenType::LBRACKET)) {
            advance(); // consume '['
            auto index_expr = std::make_unique<Expr>();
            index_expr->kind = ExprKind::INDEX;
            index_expr->line = expr->line;
            index_expr->column = expr->column;
            index_expr->index.object = std::move(expr);

            // Check if this is a slice (has ':' inside)
            if (check(TokenType::COLON)) {
                // Slice: arr[:end] or arr[start:] or arr[start:end]
                index_expr->kind = ExprKind::SLICE;
                index_expr->slice.start = nullptr;
                if (!check(TokenType::COLON)) {
                    index_expr->slice.start = parse_expr();
                }
                expect(TokenType::COLON, "expected ':'");
                if (!check(TokenType::RBRACKET)) {
                    index_expr->slice.end = parse_expr();
                }
                index_expr->slice.object = std::move(index_expr->index.object);
            } else {
                index_expr->index.index = parse_expr();
                // Check for slice: [start:end]
                if (match(TokenType::COLON)) {
                    index_expr->kind = ExprKind::SLICE;
                    index_expr->slice.start = std::move(index_expr->index.index);
                    if (!check(TokenType::RBRACKET)) {
                        index_expr->slice.end = parse_expr();
                    }
                    index_expr->slice.object = std::move(index_expr->index.object);
                }
            }

            expect(TokenType::RBRACKET, "expected ']'");
            expr = std::move(index_expr);
            continue;
        }

        // Dot access: expr.field
        if (check(TokenType::DOT)) {
            advance(); // consume '.'
            Token field = expect(TokenType::IDENT, "expected field name");
            auto dot_expr = std::make_unique<Expr>();
            dot_expr->kind = ExprKind::DOT_ACCESS;
            dot_expr->line = expr->line;
            dot_expr->column = expr->column;
            dot_expr->dot.object = std::move(expr);
            dot_expr->dot.field = field.value;
            expr = std::move(dot_expr);
            continue;
        }

        // Struct literal: Type{ field: value, ... }
        // Only match uppercase identifiers (PascalCase type convention)
        if (check(TokenType::LBRACE) && expr->kind == ExprKind::IDENT &&
            !expr->ident.empty() && std::isupper(expr->ident[0])) {
            advance(); // consume '{'
            auto sl = std::make_unique<Expr>();
            sl->kind = ExprKind::STRUCT_LITERAL;
            sl->line = expr->line;
            sl->column = expr->column;
            sl->struct_literal.type_name = expr->ident;

            while (true) {
                skip_newlines();
                if (check(TokenType::RBRACE) || check(TokenType::EOF_TOKEN)) break;
                StructFieldInit field;
                field.name = expect(TokenType::IDENT, "expected field name").value;
                expect(TokenType::COLON, "expected ':'");
                field.value = parse_expr();
                sl->struct_literal.fields.push_back(std::move(field));
                if (!check(TokenType::RBRACE)) {
                    expect(TokenType::COMMA, "expected ',' or '}'");
                }
            }
            expect(TokenType::RBRACE, "expected '}'");
            expr = std::move(sl);
            continue;
        }

        // Postfix inc/dec: expr++ / expr--
        if (check(TokenType::PLUS_PLUS) || check(TokenType::MINUS_MINUS)) {
            Token op = advance();
            auto pf = std::make_unique<Expr>();
            pf->kind = (op.type == TokenType::PLUS_PLUS) ? ExprKind::POSTFIX_INC : ExprKind::POSTFIX_DEC;
            pf->line = expr->line;
            pf->column = expr->column;
            pf->postfix.operand = std::move(expr);
            expr = std::move(pf);
            break;
        }

        break;
    }

    return expr;
}

ExprPtr Parser::parse_call_args(ExprPtr callee) {
    advance(); // consume '('
    auto expr = std::make_unique<Expr>();
    expr->kind = ExprKind::CALL;
    expr->line = callee->line;
    expr->column = callee->column;
    expr->call.callee = std::move(callee);

    if (!check(TokenType::RPAREN)) {
        expr->call.args.push_back(parse_expr());
        while (match(TokenType::COMMA)) {
            expr->call.args.push_back(parse_expr());
        }
    }

    expect(TokenType::RPAREN, "expected ')'");
    return expr;
}

ExprPtr Parser::parse_primary() {
    // Integer literal
    if (check(TokenType::INT_LIT)) {
        Token tok = advance();
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::INT_LIT;
        expr->line = tok.line;
        expr->column = tok.column;
        // Handle binary (0b) and hex (0x) literals
        if (tok.value.size() > 2 && tok.value[0] == '0' && tok.value[1] == 'b') {
            expr->int_val = std::stoll(tok.value.substr(2), nullptr, 2);
        } else if (tok.value.size() > 2 && tok.value[0] == '0' && tok.value[1] == 'x') {
            expr->int_val = std::stoll(tok.value.substr(2), nullptr, 16);
        } else {
            expr->int_val = std::stoll(tok.value);
        }
        return expr;
    }

    // Float literal
    if (check(TokenType::FLOAT_LIT)) {
        Token tok = advance();
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::FLOAT_LIT;
        expr->line = tok.line;
        expr->column = tok.column;
        expr->float_val = std::stod(tok.value);
        return expr;
    }

    // String literal
    if (check(TokenType::STRING_LIT)) {
        Token tok = advance();
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::STRING_LIT;
        expr->line = tok.line;
        expr->column = tok.column;
        expr->string_val = tok.value;
        return expr;
    }

    // Char literal
    if (check(TokenType::CHAR_LIT)) {
        Token tok = advance();
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::CHAR_LIT;
        expr->line = tok.line;
        expr->column = tok.column;
        expr->char_val = tok.value.empty() ? 0 : tok.value[0];
        return expr;
    }

    // Bool literal
    if (check(TokenType::KW_TRUE) || check(TokenType::KW_FALSE)) {
        Token tok = advance();
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::BOOL_LIT;
        expr->line = tok.line;
        expr->column = tok.column;
        expr->bool_val = (tok.type == TokenType::KW_TRUE);
        return expr;
    }

    // Nil literal
    if (check(TokenType::KW_NIL)) {
        Token tok = advance();
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::NIL_LIT;
        expr->line = tok.line;
        expr->column = tok.column;
        return expr;
    }

    // Identifier
    if (check(TokenType::IDENT)) {
        Token tok = advance();
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::IDENT;
        expr->line = tok.line;
        expr->column = tok.column;
        expr->ident = tok.value;
        return expr;
    }

    // Parenthesized expression
    if (match(TokenType::LPAREN)) {
        auto expr = parse_expr();
        expect(TokenType::RPAREN, "expected ')'");
        return expr;
    }

    error("unexpected token in expression");
}

std::string binop_str(BinOp op) {
    switch (op) {
        case BinOp::ADD: return "+";
        case BinOp::SUB: return "-";
        case BinOp::MUL: return "*";
        case BinOp::DIV: return "/";
        case BinOp::MOD: return "%";
        case BinOp::EQ: return "==";
        case BinOp::NEQ: return "!=";
        case BinOp::LT: return "<";
        case BinOp::GT: return ">";
        case BinOp::LTE: return "<=";
        case BinOp::GTE: return ">=";
        case BinOp::AND: return "&&";
        case BinOp::OR: return "||";
        case BinOp::BIT_AND: return "&";
        case BinOp::BIT_OR: return "|";
        case BinOp::BIT_XOR: return "^";
        case BinOp::LSHIFT: return "<<";
        case BinOp::RSHIFT: return ">>";
    }
    return "?";
}

std::string unop_str(UnOp op) {
    switch (op) {
        case UnOp::NEG: return "-";
        case UnOp::NOT: return "!";
        case UnOp::BIT_NOT: return "~";
        case UnOp::DEREF: return "*";
        case UnOp::ADDR_OF: return "&";
    }
    return "?";
}

// ==================== Clone Utilities ====================

static TypePtr clone_type(const TypePtr& src) {
    if (!src) return nullptr;
    auto dst = std::make_unique<TypeAnnotation>();
    dst->kind = src->kind;
    dst->name = src->name;
    dst->array_size = src->array_size;
    dst->array_size_name = src->array_size_name;
    dst->inner = clone_type(src->inner);
    for (auto& p : src->fn_params) {
        FnParam fp;
        fp.name = p.name;
        fp.type = clone_type(p.type);
        dst->fn_params.push_back(std::move(fp));
    }
    for (auto& r : src->fn_returns) {
        dst->fn_returns.push_back(clone_type(r));
    }
    return dst;
}

ExprPtr clone_expr(const ExprPtr& src) {
    if (!src) return nullptr;
    auto dst = std::make_unique<Expr>();
    dst->kind = src->kind;
    dst->line = src->line;
    dst->column = src->column;
    switch (src->kind) {
        case ExprKind::INT_LIT:
            dst->int_val = src->int_val;
            break;
        case ExprKind::FLOAT_LIT:
            dst->float_val = src->float_val;
            break;
        case ExprKind::STRING_LIT:
            dst->string_val = src->string_val;
            break;
        case ExprKind::CHAR_LIT:
            dst->char_val = src->char_val;
            break;
        case ExprKind::BOOL_LIT:
            dst->bool_val = src->bool_val;
            break;
        case ExprKind::NIL_LIT:
            break;
        case ExprKind::IDENT:
            dst->ident = src->ident;
            break;
        case ExprKind::BINARY:
            dst->binary.op = src->binary.op;
            dst->binary.left = clone_expr(src->binary.left);
            dst->binary.right = clone_expr(src->binary.right);
            break;
        case ExprKind::UNARY:
            dst->unary.op = src->unary.op;
            dst->unary.operand = clone_expr(src->unary.operand);
            break;
        case ExprKind::ASSIGN:
            dst->assign.target = clone_expr(src->assign.target);
            dst->assign.value = clone_expr(src->assign.value);
            dst->assign.is_decl = src->assign.is_decl;
            dst->assign.has_type = src->assign.has_type;
            dst->assign.type_name = src->assign.type_name;
            dst->assign.assign_op = src->assign.assign_op;
            dst->assign.raise = src->assign.raise;
            break;
        case ExprKind::CALL:
            dst->call.callee = clone_expr(src->call.callee);
            for (auto& a : src->call.args) {
                dst->call.args.push_back(clone_expr(a));
            }
            break;
        case ExprKind::INDEX:
            dst->index.object = clone_expr(src->index.object);
            dst->index.index = clone_expr(src->index.index);
            break;
        case ExprKind::SLICE:
            dst->slice.object = clone_expr(src->slice.object);
            dst->slice.start = clone_expr(src->slice.start);
            dst->slice.end = clone_expr(src->slice.end);
            break;
        case ExprKind::DOT_ACCESS:
            dst->dot.object = clone_expr(src->dot.object);
            dst->dot.field = src->dot.field;
            break;
        case ExprKind::SIZEOF:
            break;
        case ExprKind::STRUCT_LITERAL:
            dst->struct_literal.type_name = src->struct_literal.type_name;
            for (auto& f : src->struct_literal.fields) {
                StructFieldInit sf;
                sf.name = f.name;
                sf.value = clone_expr(f.value);
                dst->struct_literal.fields.push_back(std::move(sf));
            }
            break;
        case ExprKind::POSTFIX_INC:
        case ExprKind::POSTFIX_DEC:
            dst->postfix.operand = clone_expr(src->postfix.operand);
            break;
    }
    return dst;
}

StmtPtr clone_stmt(const StmtPtr& src) {
    if (!src) return nullptr;
    auto dst = std::make_unique<Stmt>();
    dst->kind = src->kind;
    dst->line = src->line;
    dst->column = src->column;
    dst->raise = src->raise;
    switch (src->kind) {
        case StmtKind::EXPR:
            dst->expr = clone_expr(src->expr);
            break;
        case StmtKind::RETURN:
            dst->expr = clone_expr(src->expr);
            for (auto& rv : src->return_values) {
                dst->return_values.push_back(clone_expr(rv));
            }
            break;
        case StmtKind::IF:
            for (auto& br : src->if_stmt.branches) {
                IfBranch ib;
                ib.condition = clone_expr(br.condition);
                for (auto& s : br.body) {
                    ib.body.push_back(clone_stmt(s));
                }
                dst->if_stmt.branches.push_back(std::move(ib));
            }
            for (auto& s : src->if_stmt.else_body) {
                dst->if_stmt.else_body.push_back(clone_stmt(s));
            }
            break;
        case StmtKind::FOR:
            dst->for_stmt.init = clone_stmt(src->for_stmt.init);
            dst->for_stmt.cond = clone_expr(src->for_stmt.cond);
            dst->for_stmt.update = clone_expr(src->for_stmt.update);
            for (auto& s : src->for_stmt.body) {
                dst->for_stmt.body.push_back(clone_stmt(s));
            }
            break;
        case StmtKind::WHILE:
            dst->while_stmt.condition = clone_expr(src->while_stmt.condition);
            for (auto& s : src->while_stmt.body) {
                dst->while_stmt.body.push_back(clone_stmt(s));
            }
            break;
        case StmtKind::FOR_RANGE:
            dst->range_stmt.index_name = src->range_stmt.index_name;
            dst->range_stmt.value_name = src->range_stmt.value_name;
            dst->range_stmt.range_expr = clone_expr(src->range_stmt.range_expr);
            for (auto& s : src->range_stmt.body) {
                dst->range_stmt.body.push_back(clone_stmt(s));
            }
            break;
        case StmtKind::BLOCK:
            for (auto& s : src->block.stmts) {
                dst->block.stmts.push_back(clone_stmt(s));
            }
            break;
        case StmtKind::BREAK:
        case StmtKind::CONTINUE:
            break;
        case StmtKind::DEFER:
            dst->defer_stmt.expr = clone_expr(src->defer_stmt.expr);
            break;
        case StmtKind::SWITCH:
            dst->switch_stmt.value = clone_expr(src->switch_stmt.value);
            for (auto& c : src->switch_stmt.cases) {
                SwitchCase sc;
                sc.is_default = c.is_default;
                for (auto& v : c.values) {
                    sc.values.push_back(clone_expr(v));
                }
                for (auto& s : c.body) {
                    sc.body.push_back(clone_stmt(s));
                }
                dst->switch_stmt.cases.push_back(std::move(sc));
            }
            break;
        case StmtKind::ASM:
            dst->asm_stmt.code = src->asm_stmt.code;
            dst->asm_stmt.is_volatile = src->asm_stmt.is_volatile;
            dst->asm_stmt.clobbers = src->asm_stmt.clobbers;
            for (auto& o : src->asm_stmt.outputs) {
                AsmOperand op;
                op.constraint = o.constraint;
                op.name = o.name;
                op.value = clone_expr(o.value);
                op.is_output = o.is_output;
                dst->asm_stmt.outputs.push_back(std::move(op));
            }
            for (auto& i : src->asm_stmt.inputs) {
                AsmOperand op;
                op.constraint = i.constraint;
                op.name = i.name;
                op.value = clone_expr(i.value);
                op.is_output = i.is_output;
                dst->asm_stmt.inputs.push_back(std::move(op));
            }
            break;
        case StmtKind::MULTI_ASSIGN:
            dst->multi_assign.targets = src->multi_assign.targets;
            dst->multi_assign.value = clone_expr(src->multi_assign.value);
            dst->multi_assign.is_decl = src->multi_assign.is_decl;
            dst->multi_assign.type_names = src->multi_assign.type_names;
            break;
    }
    return dst;
}

} // namespace binar
