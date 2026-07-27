#pragma once

#include "ast.h"
#include "config.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace binar {

struct TypeInfo {
    std::string name;
    bool is_pointer;
    bool is_slice;
    bool is_function;
    TypePtr ast_type;
};

struct VarInfo {
    std::string name;
    std::string type_name;
    bool is_mutable;
    bool is_param;
};

struct FnInfo {
    std::string name;
    std::vector<std::string> param_types;
    std::vector<std::string> return_types;
    bool is_method;  // first param is a type
};

struct TypeInfoFull {
    std::string name;
    std::vector<std::pair<std::string, std::string>> fields; // name, type
};

struct SemaError {
    std::string message;
    int line;
    int column;
};

class Sema {
public:
    explicit Sema(const CompilerConfig& cfg = CompilerConfig::default_config());
    bool analyze(Program& program);
    const std::vector<SemaError>& errors() const { return errors_; }

private:
    void check_decl(Decl& decl);
    void check_fn_decl(FnDecl& fn, int line, int column);
    void check_type_decl(TypeDecl& td, int line, int column);
    void check_constant_decl(ConstantDecl& cd, int line, int column);
    void check_stmt(Stmt& stmt);
    void check_expr(Expr& expr);
    std::string resolve_type(TypeAnnotation& type);
    void error(const std::string& msg, int line, int column);
    void check_structural_methods(FnDecl& fn);

    std::vector<SemaError> errors_;
    std::unordered_map<std::string, TypeInfoFull> types_;
    std::unordered_map<std::string, std::vector<FnInfo>> functions_;
    std::vector<std::unordered_map<std::string, VarInfo>> scopes_;

    // Raise enforcement
    void check_raise_syntax(Program& program);
    bool fn_returns_error(const std::string& fn_name, Program& program);

    CompilerConfig config_;
};

} // namespace binar
