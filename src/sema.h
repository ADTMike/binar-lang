#pragma once

#include "ast.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <set>
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

struct InterfaceInfo {
    std::string name;
    std::vector<InterfaceMethod> methods;
};

struct SemaError {
    std::string message;
    int line;
    int column;
};

class Sema {
public:
    bool analyze(Program& program);
    const std::vector<SemaError>& errors() const { return errors_; }

private:
    void check_decl(Decl& decl);
    void check_fn_decl(FnDecl& fn, int line, int column);
    void check_type_decl(TypeDecl& td, int line, int column);
    void check_iface_decl(InterfaceDecl& id, int line, int column);
    void check_const_decl(ConstDecl& cd, int line, int column);
    void check_stmt(Stmt& stmt);
    void check_expr(Expr& expr);
    std::string resolve_type(TypeAnnotation& type);
    void error(const std::string& msg, int line, int column);

    // Recursion detection helpers
    void build_call_graph(Program& program);
    bool has_cycle_dfs(const std::string& fn, std::set<std::string>& visited, std::vector<std::string>& path);
    void collect_calls_from_expr(Expr* expr, std::set<std::string>& calls);
    void collect_calls_from_stmt(Stmt* stmt, std::set<std::string>& calls);
    bool is_tail_recursive(FnDecl& fn);
    bool is_tail_recursive_stmt(Stmt* stmt, const std::string& fn_name);
    bool is_tail_recursive_expr(Expr* expr, const std::string& fn_name);
    void transform_tail_recursion(FnDecl& fn);

    std::vector<SemaError> errors_;
    std::unordered_map<std::string, TypeInfoFull> types_;
    std::unordered_map<std::string, InterfaceInfo> interfaces_;
    std::unordered_map<std::string, std::vector<FnInfo>> functions_;
    std::vector<std::unordered_map<std::string, VarInfo>> scopes_;
    std::set<std::string> interface_returning_fns_;  // functions returning interface (error, user iface)
    std::unordered_map<std::string, std::set<std::string>> call_graph_;  // fn_name -> set of called iface-returning fns

    // Raise enforcement
    void check_raise_syntax(Program& program);
    int get_fn_return_count(const std::string& fn_name, Program& program);
    bool is_error_or_interface_type(const std::string& type_name);
};

} // namespace binar
