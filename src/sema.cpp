#include "sema.h"
#include <set>

namespace binar {

bool Sema::analyze(Program& program) {
    // First pass: collect type and interface declarations
    for (auto& decl : program.decls) {
        if (decl.kind == DeclKind::TYPE) {
            // Check for interface/struct name collision
            if (interfaces_.count(decl.type_decl.name)) {
                error("type '" + decl.type_decl.name + "' conflicts with interface of same name",
                      decl.line, decl.column);
            }
            TypeInfoFull info;
            info.name = decl.type_decl.name;
            for (auto& field : decl.type_decl.fields) {
                info.fields.push_back({field.name, ""});
            }
            types_[decl.type_decl.name] = info;
        } else if (decl.kind == DeclKind::IFACE) {
            // Check for interface/struct name collision
            if (types_.count(decl.iface_decl.name)) {
                error("interface '" + decl.iface_decl.name + "' conflicts with type of same name",
                      decl.line, decl.column);
            }
            InterfaceInfo info;
            info.name = decl.iface_decl.name;
            info.methods = std::move(decl.iface_decl.methods);
            interfaces_[decl.iface_decl.name] = std::move(info);
        }
    }

    // Second pass: check declarations
    for (auto& decl : program.decls) {
        check_decl(decl);
    }

    // Third pass: tag interface-returning functions
    for (auto& decl : program.decls) {
        if (decl.kind == DeclKind::FN) {
            bool is_iface_return = false;
            for (auto& rt : decl.fn.return_types) {
                std::string rtname = rt->name;
                if (rtname == "error" || interfaces_.count(rtname)) {
                    is_iface_return = true;
                    break;
                }
            }
            if (is_iface_return) {
                decl.fn.is_interface_returning = true;
                interface_returning_fns_.insert(decl.fn.name);
            }
        }
    }

    // Fourth pass: recursion detection for interface-returning functions
    if (errors_.empty()) {
        build_call_graph(program);

        // Detect mutual recursion (cycles in call graph)
        std::set<std::string> visited;
        std::vector<std::string> path;
        for (auto& [fn_name, _] : call_graph_) {
            if (visited.count(fn_name) == 0) {
                if (has_cycle_dfs(fn_name, visited, path)) {
                    // Build cycle string from path
                    std::string cycle;
                    // path contains the cycle - find where it repeats
                    std::string cycle_start = path.back();
                    for (auto& fn : path) {
                        if (!cycle.empty()) cycle += " -> ";
                        cycle += fn;
                        if (fn == cycle_start && cycle.size() > cycle_start.size()) break;
                    }
                    // Find the decl for line info
                    for (auto& decl : program.decls) {
                        if (decl.kind == DeclKind::FN && decl.fn.name == fn_name) {
                            error("mutual recursion detected among interface-returning functions: " + cycle,
                                  decl.line, decl.column);
                            break;
                        }
                    }
                    break;
                }
            }
        }

        // Detect and transform tail recursion (only for non-mutual cases)
        if (errors_.empty()) {
            for (auto& decl : program.decls) {
                if (decl.kind == DeclKind::FN && decl.fn.is_interface_returning) {
                    if (is_tail_recursive(decl.fn)) {
                        transform_tail_recursion(decl.fn);
                    }
                }
            }
        }
    }

    // Fifth pass: enforce raise syntax rules
    if (errors_.empty()) {
        check_raise_syntax(program);
    }

    return errors_.empty();
}

void Sema::check_decl(Decl& decl) {
    switch (decl.kind) {
        case DeclKind::FN:
            check_fn_decl(decl.fn, decl.line, decl.column);
            break;
        case DeclKind::TYPE:
            check_type_decl(decl.type_decl, decl.line, decl.column);
            break;
        case DeclKind::IFACE:
            check_iface_decl(decl.iface_decl, decl.line, decl.column);
            break;
        case DeclKind::CONST:
            check_const_decl(decl.const_decl, decl.line, decl.column);
            break;
        case DeclKind::IMPORT:
            // Imports are checked later during resolution
            break;
    }
}

void Sema::check_fn_decl(FnDecl& fn, int line, int column) {
    // Enter new scope
    scopes_.push_back({});

    // Add parameters to scope
    for (auto& param : fn.params) {
        VarInfo var;
        var.name = param.name;
        var.type_name = param.type ? param.type->name : "unknown";
        var.is_mutable = true;
        var.is_param = true;
        scopes_.back()[param.name] = var;
    }

    // Check if this function satisfies any interface method
    for (auto& [iname, iinfo] : interfaces_) {
        for (auto& imethod : iinfo.methods) {
            if (fn.name == imethod.name && fn.params.size() >= 1) {
                // Check parameter type matches
                TypeAnnotation* param_type = fn.params[0].type.get();
                bool fn_is_pointer = (param_type->kind == TypeKind::POINTER);

                if (fn_is_pointer != imethod.is_pointer) {
                    error("function '" + fn.name + "' pointer/non-pointer mismatch "
                          "for interface '" + iname + "'",
                          line, column);
                }
            }
        }
    }

    // Check body
    for (auto& stmt : fn.body) {
        check_stmt(*stmt);
    }

    // Exit scope
    scopes_.pop_back();
}

void Sema::check_type_decl(TypeDecl& td, int line, int column) {
    // Type declarations are mostly checked in the first pass
}

void Sema::check_iface_decl(InterfaceDecl& id, int line, int column) {
    // Check for duplicate methods
    std::set<std::string> seen;
    for (auto& method : id.methods) {
        if (seen.count(method.name)) {
            error("duplicate method '" + method.name + "' in interface '" + id.name + "'",
                  line, column);
        }
        seen.insert(method.name);
    }
}

void Sema::check_const_decl(ConstDecl& cd, int line, int column) {
    if (cd.value) {
        check_expr(*cd.value);
    }
}

void Sema::check_stmt(Stmt& stmt) {
    switch (stmt.kind) {
        case StmtKind::EXPR:
            if (stmt.expr) {
                check_expr(*stmt.expr);
                if (stmt.expr->kind == ExprKind::ASSIGN && stmt.expr->assign.is_decl) {
                    VarInfo var;
                    var.name = stmt.expr->assign.target->ident;
                    var.type_name = stmt.expr->assign.has_type ? stmt.expr->assign.type_name : "";
                    var.is_mutable = true;
                    var.is_param = false;
                    scopes_.back()[var.name] = var;
                }
            }
            break;
        case StmtKind::RETURN:
            if (stmt.expr) check_expr(*stmt.expr);
            for (auto& rv : stmt.return_values) {
                check_expr(*rv);
                if (rv->kind == ExprKind::UNARY && rv->unary.op == UnOp::ADDR_OF) {
                    if (rv->unary.operand->kind == ExprKind::IDENT) {
                        std::string name = rv->unary.operand->ident;
                        for (int i = scopes_.size() - 1; i >= 0; i--) {
                            auto it = scopes_[i].find(name);
                            if (it != scopes_[i].end()) {
                                if (!it->second.is_param) {
                                    error("cannot return pointer to local variable '" + name + "'",
                                        rv->line, rv->column);
                                }
                                break;
                            }
                        }
                    }
                }
            }
            if (stmt.expr && stmt.expr->kind == ExprKind::UNARY && stmt.expr->unary.op == UnOp::ADDR_OF) {
                if (stmt.expr->unary.operand->kind == ExprKind::IDENT) {
                    std::string name = stmt.expr->unary.operand->ident;
                    for (int i = scopes_.size() - 1; i >= 0; i--) {
                        auto it = scopes_[i].find(name);
                        if (it != scopes_[i].end()) {
                            if (!it->second.is_param) {
                                error("cannot return pointer to local variable '" + name + "'",
                                    stmt.expr->line, stmt.expr->column);
                            }
                            break;
                        }
                    }
                }
            }
            break;
        case StmtKind::IF:
            for (auto& branch : stmt.if_stmt.branches) {
                check_expr(*branch.condition);
                for (auto& s : branch.body) check_stmt(*s);
            }
            for (auto& s : stmt.if_stmt.else_body) check_stmt(*s);
            break;
        case StmtKind::FOR:
            scopes_.push_back({});
            if (stmt.for_stmt.init) check_stmt(*stmt.for_stmt.init);
            if (stmt.for_stmt.cond) check_expr(*stmt.for_stmt.cond);
            if (stmt.for_stmt.update) check_expr(*stmt.for_stmt.update);
            for (auto& s : stmt.for_stmt.body) check_stmt(*s);
            scopes_.pop_back();
            break;
        case StmtKind::WHILE:
            check_expr(*stmt.while_stmt.condition);
            for (auto& s : stmt.while_stmt.body) check_stmt(*s);
            break;
        case StmtKind::FOR_RANGE:
            scopes_.push_back({});
            {
                VarInfo idx_var;
                idx_var.name = stmt.range_stmt.index_name;
                idx_var.type_name = "int";
                idx_var.is_mutable = false;
                if (stmt.range_stmt.index_name != "_") {
                    scopes_.back()[stmt.range_stmt.index_name] = idx_var;
                }

                if (!stmt.range_stmt.value_name.empty() && stmt.range_stmt.value_name != "_") {
                    VarInfo val_var;
                    val_var.name = stmt.range_stmt.value_name;
                    val_var.type_name = "unknown";
                    val_var.is_mutable = false;
                    scopes_.back()[stmt.range_stmt.value_name] = val_var;
                }

                check_expr(*stmt.range_stmt.range_expr);
                for (auto& s : stmt.range_stmt.body) check_stmt(*s);
            }
            scopes_.pop_back();
            break;
        case StmtKind::BLOCK:
            scopes_.push_back({});
            for (auto& s : stmt.block.stmts) check_stmt(*s);
            scopes_.pop_back();
            break;
        case StmtKind::BREAK:
        case StmtKind::CONTINUE:
            break;
        case StmtKind::DEFER:
            if (stmt.defer_stmt.expr) check_expr(*stmt.defer_stmt.expr);
            break;
        case StmtKind::SWITCH:
            if (stmt.switch_stmt.value) check_expr(*stmt.switch_stmt.value);
            for (auto& sc : stmt.switch_stmt.cases) {
                for (auto& val : sc.values) check_expr(*val);
                for (auto& s : sc.body) check_stmt(*s);
            }
            break;
        case StmtKind::ASM:
            break;
    }
}

void Sema::check_expr(Expr& expr) {
    switch (expr.kind) {
        case ExprKind::INT_LIT:
        case ExprKind::FLOAT_LIT:
        case ExprKind::STRING_LIT:
        case ExprKind::CHAR_LIT:
        case ExprKind::BOOL_LIT:
        case ExprKind::NIL_LIT:
            break;
        case ExprKind::IDENT:
            // TODO: check if identifier exists in scope
            break;
        case ExprKind::BINARY:
            check_expr(*expr.binary.left);
            check_expr(*expr.binary.right);
            break;
        case ExprKind::UNARY:
            check_expr(*expr.unary.operand);
            break;
        case ExprKind::ASSIGN:
            check_expr(*expr.assign.target);
            if (expr.assign.value) check_expr(*expr.assign.value);
            break;
        case ExprKind::CALL:
            check_expr(*expr.call.callee);
            for (auto& arg : expr.call.args) check_expr(*arg);
            break;
        case ExprKind::INDEX:
            check_expr(*expr.index.object);
            check_expr(*expr.index.index);
            break;
        case ExprKind::SLICE:
            check_expr(*expr.slice.object);
            if (expr.slice.start) check_expr(*expr.slice.start);
            if (expr.slice.end) check_expr(*expr.slice.end);
            break;
        case ExprKind::DOT_ACCESS:
            check_expr(*expr.dot.object);
            break;
        case ExprKind::SIZEOF:
            break;
    }
}

std::string Sema::resolve_type(TypeAnnotation& type) {
    switch (type.kind) {
        case TypeKind::NAMED:
            return type.name;
        case TypeKind::POINTER:
            return "*" + resolve_type(*type.inner);
        case TypeKind::SLICE:
            return "[]" + resolve_type(*type.inner);
        case TypeKind::ARRAY:
            return "[" + std::to_string(type.array_size) + "]" + resolve_type(*type.inner);
        default:
            return "unknown";
    }
}

void Sema::error(const std::string& msg, int line, int column) {
    errors_.push_back({msg, line, column});
}

// ==================== Recursion Detection ====================

void Sema::collect_calls_from_expr(Expr* expr, std::set<std::string>& calls) {
    if (!expr) return;
    switch (expr->kind) {
        case ExprKind::CALL:
            if (expr->call.callee && expr->call.callee->kind == ExprKind::IDENT) {
                calls.insert(expr->call.callee->ident);
            }
            for (auto& arg : expr->call.args) {
                collect_calls_from_expr(arg.get(), calls);
            }
            break;
        case ExprKind::BINARY:
            collect_calls_from_expr(expr->binary.left.get(), calls);
            collect_calls_from_expr(expr->binary.right.get(), calls);
            break;
        case ExprKind::UNARY:
            collect_calls_from_expr(expr->unary.operand.get(), calls);
            break;
        case ExprKind::ASSIGN:
            collect_calls_from_expr(expr->assign.target.get(), calls);
            collect_calls_from_expr(expr->assign.value.get(), calls);
            break;
        case ExprKind::INDEX:
            collect_calls_from_expr(expr->index.object.get(), calls);
            collect_calls_from_expr(expr->index.index.get(), calls);
            break;
        case ExprKind::SLICE:
            collect_calls_from_expr(expr->slice.object.get(), calls);
            if (expr->slice.start) collect_calls_from_expr(expr->slice.start.get(), calls);
            if (expr->slice.end) collect_calls_from_expr(expr->slice.end.get(), calls);
            break;
        case ExprKind::DOT_ACCESS:
            collect_calls_from_expr(expr->dot.object.get(), calls);
            break;
        default:
            break;
    }
}

void Sema::collect_calls_from_stmt(Stmt* stmt, std::set<std::string>& calls) {
    if (!stmt) return;
    switch (stmt->kind) {
        case StmtKind::EXPR:
            collect_calls_from_expr(stmt->expr.get(), calls);
            break;
        case StmtKind::RETURN:
            collect_calls_from_expr(stmt->expr.get(), calls);
            for (auto& rv : stmt->return_values) {
                collect_calls_from_expr(rv.get(), calls);
            }
            break;
        case StmtKind::IF:
            for (auto& branch : stmt->if_stmt.branches) {
                collect_calls_from_expr(branch.condition.get(), calls);
                for (auto& s : branch.body) {
                    collect_calls_from_stmt(s.get(), calls);
                }
            }
            for (auto& s : stmt->if_stmt.else_body) {
                collect_calls_from_stmt(s.get(), calls);
            }
            break;
        case StmtKind::FOR:
            if (stmt->for_stmt.init) collect_calls_from_stmt(stmt->for_stmt.init.get(), calls);
            if (stmt->for_stmt.cond) collect_calls_from_expr(stmt->for_stmt.cond.get(), calls);
            if (stmt->for_stmt.update) collect_calls_from_expr(stmt->for_stmt.update.get(), calls);
            for (auto& s : stmt->for_stmt.body) {
                collect_calls_from_stmt(s.get(), calls);
            }
            break;
        case StmtKind::WHILE:
            collect_calls_from_expr(stmt->while_stmt.condition.get(), calls);
            for (auto& s : stmt->while_stmt.body) {
                collect_calls_from_stmt(s.get(), calls);
            }
            break;
        case StmtKind::FOR_RANGE:
            collect_calls_from_expr(stmt->range_stmt.range_expr.get(), calls);
            for (auto& s : stmt->range_stmt.body) {
                collect_calls_from_stmt(s.get(), calls);
            }
            break;
        case StmtKind::DEFER:
            collect_calls_from_expr(stmt->defer_stmt.expr.get(), calls);
            break;
        case StmtKind::SWITCH:
            if (stmt->switch_stmt.value) {
                collect_calls_from_expr(stmt->switch_stmt.value.get(), calls);
            }
            for (auto& sc : stmt->switch_stmt.cases) {
                for (auto& v : sc.values) {
                    collect_calls_from_expr(v.get(), calls);
                }
                for (auto& s : sc.body) {
                    collect_calls_from_stmt(s.get(), calls);
                }
            }
            break;
        default:
            break;
    }
}

void Sema::build_call_graph(Program& program) {
    for (auto& decl : program.decls) {
        if (decl.kind != DeclKind::FN || !decl.fn.is_interface_returning) continue;

        std::set<std::string> calls;
        for (auto& stmt : decl.fn.body) {
            collect_calls_from_stmt(stmt.get(), calls);
        }

        // Filter to only include calls to other interface-returning functions
        for (auto& callee : calls) {
            if (interface_returning_fns_.count(callee) && callee != decl.fn.name) {
                call_graph_[decl.fn.name].insert(callee);
            }
        }
    }
}

bool Sema::has_cycle_dfs(const std::string& fn, std::set<std::string>& visited, std::vector<std::string>& path) {
    visited.insert(fn);
    path.push_back(fn);

    auto it = call_graph_.find(fn);
    if (it != call_graph_.end()) {
        for (auto& callee : it->second) {
            if (visited.count(callee) == 0) {
                if (has_cycle_dfs(callee, visited, path)) {
                    return true;
                }
            } else {
                // Check if callee is in current path (cycle)
                for (auto& p : path) {
                    if (p == callee) {
                        // Found cycle - add callee to path to complete the cycle display
                        path.push_back(callee);
                        return true;
                    }
                }
            }
        }
    }

    path.pop_back();
    return false;
}

bool Sema::is_tail_recursive_expr(Expr* expr, const std::string& fn_name) {
    if (!expr) return false;
    // A tail call is a direct call to the same function
    return expr->kind == ExprKind::CALL &&
           expr->call.callee &&
           expr->call.callee->kind == ExprKind::IDENT &&
           expr->call.callee->ident == fn_name;
}

bool Sema::is_tail_recursive_stmt(Stmt* stmt, const std::string& fn_name) {
    if (!stmt) return false;

    switch (stmt->kind) {
        case StmtKind::RETURN:
            // Direct return of a call to self = tail recursive
            return is_tail_recursive_expr(stmt->expr.get(), fn_name);

        case StmtKind::IF:
            // If ALL branches end with tail calls, it's tail recursive
            // Check each if branch
            for (auto& branch : stmt->if_stmt.branches) {
                if (branch.body.empty()) return false;
                Stmt* last = branch.body.back().get();
                if (!is_tail_recursive_stmt(last, fn_name)) return false;
            }
            // Check else branch
            if (!stmt->if_stmt.else_body.empty()) {
                Stmt* last = stmt->if_stmt.else_body.back().get();
                if (!is_tail_recursive_stmt(last, fn_name)) return false;
            } else {
                return false;
            }
            return true;

        case StmtKind::BLOCK:
            if (stmt->block.stmts.empty()) return false;
            return is_tail_recursive_stmt(stmt->block.stmts.back().get(), fn_name);

        default:
            return false;
    }
}

// Helper: check if a branch is "safe" (either a tail call to self or a non-recursive return)
static bool is_branch_safe(Stmt* stmt, const std::string& fn_name) {
    if (!stmt) return false;

    switch (stmt->kind) {
        case StmtKind::RETURN:
            // Any return is safe: either a tail call to self, or a base case
            return true;

        case StmtKind::IF:
            // All branches must be safe
            for (auto& branch : stmt->if_stmt.branches) {
                if (branch.body.empty()) return false;
                if (!is_branch_safe(branch.body.back().get(), fn_name)) return false;
            }
            if (!stmt->if_stmt.else_body.empty()) {
                if (!is_branch_safe(stmt->if_stmt.else_body.back().get(), fn_name)) return false;
            } else {
                // No else branch: only safe if the if-body doesn't contain recursive calls
                // (it's just a guard/base case)
                // We can't easily check this, so we consider it safe only if there's no
                // recursive call in the if body - but for simplicity, return true
                // since the main flow continues after the if
                return true;
            }
            return true;

        case StmtKind::BLOCK:
            if (stmt->block.stmts.empty()) return false;
            return is_branch_safe(stmt->block.stmts.back().get(), fn_name);

        default:
            return false;
    }
}

// Helper: check if a statement contains a non-tail self-recursive call
static bool has_non_tail_recursive_call(Stmt* stmt, const std::string& fn_name) {
    if (!stmt) return false;

    switch (stmt->kind) {
        case StmtKind::EXPR:
            // Check if the expression contains a self-recursive call
            if (stmt->expr && stmt->expr->kind == ExprKind::CALL &&
                stmt->expr->call.callee && stmt->expr->call.callee->kind == ExprKind::IDENT &&
                stmt->expr->call.callee->ident == fn_name) {
                return true; // Call in expression context, not tail position
            }
            break;

        case StmtKind::RETURN:
            if (stmt->expr) {
                // Direct return of call to self = tail call (OK)
                if (stmt->expr->kind == ExprKind::CALL &&
                    stmt->expr->call.callee && stmt->expr->call.callee->kind == ExprKind::IDENT &&
                    stmt->expr->call.callee->ident == fn_name) {
                    return false; // Tail call, OK
                }
                // Check if the return expression CONTAINS a self-recursive call
                // e.g., return f(n-1) + 1 — the call is nested in a binary expr
                {
                    // Check if any sub-expression is a call to fn_name
                    auto check_expr = [&](auto& self, Expr* e) -> bool {
                        if (!e) return false;
                        if (e->kind == ExprKind::CALL && e->call.callee &&
                            e->call.callee->kind == ExprKind::IDENT &&
                            e->call.callee->ident == fn_name) {
                            return true; // Found non-tail call
                        }
                        switch (e->kind) {
                            case ExprKind::BINARY:
                                return self(self, e->binary.left.get()) || self(self, e->binary.right.get());
                            case ExprKind::UNARY:
                                return self(self, e->unary.operand.get());
                            case ExprKind::ASSIGN:
                                return self(self, e->assign.target.get()) || self(self, e->assign.value.get());
                            case ExprKind::CALL:
                                for (auto& a : e->call.args) {
                                    if (self(self, a.get())) return true;
                                }
                                return false;
                            case ExprKind::INDEX:
                                return self(self, e->index.object.get()) || self(self, e->index.index.get());
                            case ExprKind::DOT_ACCESS:
                                return self(self, e->dot.object.get());
                            default:
                                return false;
                        }
                    };
                    if (check_expr(check_expr, stmt->expr.get())) {
                        return true; // Non-tail call found in return expression
                    }
                }
                return false; // No recursive calls in return expression (base case)
            }
            break;

        case StmtKind::IF:
            for (auto& branch : stmt->if_stmt.branches) {
                for (auto& s : branch.body) {
                    if (has_non_tail_recursive_call(s.get(), fn_name)) return true;
                }
            }
            for (auto& s : stmt->if_stmt.else_body) {
                if (has_non_tail_recursive_call(s.get(), fn_name)) return true;
            }
            return false;

        case StmtKind::FOR:
        case StmtKind::WHILE:
        case StmtKind::FOR_RANGE:
            // Loops with recursive calls are non-tail
            return true;

        case StmtKind::BLOCK:
            for (auto& s : stmt->block.stmts) {
                if (has_non_tail_recursive_call(s.get(), fn_name)) return true;
            }
            return false;

        case StmtKind::DEFER:
            if (stmt->defer_stmt.expr && stmt->defer_stmt.expr->kind == ExprKind::CALL &&
                stmt->defer_stmt.expr->call.callee && stmt->defer_stmt.expr->call.callee->kind == ExprKind::IDENT &&
                stmt->defer_stmt.expr->call.callee->ident == fn_name) {
                return true; // Recursive call in defer is non-tail
            }
            break;

        default:
            break;
    }
    return false;
}

bool Sema::is_tail_recursive(FnDecl& fn) {
    // Check if this function has any direct self-recursive calls
    std::set<std::string> all_calls;
    for (auto& stmt : fn.body) {
        collect_calls_from_stmt(stmt.get(), all_calls);
    }

    // If no direct self-recursive calls, not tail recursive
    if (all_calls.count(fn.name) == 0) return false;

    // Check if there are any non-tail self-recursive calls
    for (auto& stmt : fn.body) {
        if (has_non_tail_recursive_call(stmt.get(), fn.name)) {
            // Non-tail recursion - emit error
            error("interface-returning function '" + fn.name + "' has non-tail recursive calls which would cause infinite inlining",
                  fn.body.empty() ? 0 : fn.body[0]->line,
                  fn.body.empty() ? 0 : fn.body[0]->column);
            return false;
        }
    }

    // All self-recursive calls are in tail position
    return true;
}

void Sema::transform_tail_recursion(FnDecl& fn) {
    // Transform tail recursion to a while(true) loop
    // 1. Create mutable shadow variables for each parameter
    // 2. Create a while(true) loop containing the original body
    // 3. Replace tail-call returns with variable reassignment + continue

    std::vector<StmtPtr> new_body;

    // Step 1: Create mutable shadow variables for each parameter
    for (auto& param : fn.params) {
        auto assign = std::make_unique<Expr>();
        assign->kind = ExprKind::ASSIGN;
        assign->line = 0;
        assign->column = 0;
        assign->assign.target = std::make_unique<Expr>();
        assign->assign.target->kind = ExprKind::IDENT;
        assign->assign.target->ident = param.name;
        assign->assign.value = std::make_unique<Expr>();
        assign->assign.value->kind = ExprKind::IDENT;
        assign->assign.value->ident = param.name;
        assign->assign.is_decl = true;
        assign->assign.has_type = false;
        assign->assign.assign_op = TokenType::ASSIGN;
        assign->assign.raise = false;

        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::EXPR;
        stmt->expr = std::move(assign);
        new_body.push_back(std::move(stmt));
    }

    // Step 2: Create while(true) loop
    auto while_stmt = std::make_unique<Stmt>();
    while_stmt->kind = StmtKind::WHILE;
    while_stmt->while_stmt.condition = std::make_unique<Expr>();
    while_stmt->while_stmt.condition->kind = ExprKind::BOOL_LIT;
    while_stmt->while_stmt.condition->bool_val = true;

    // Step 3: Move original body into while loop and transform tail calls
    for (auto& stmt : fn.body) {
        while_stmt->while_stmt.body.push_back(std::move(stmt));
    }

    // Now we need to walk the while body and transform tail-call returns
    // This is a recursive transformation
    auto transform_returns = [&](auto& self, std::vector<StmtPtr>& stmts) -> void {
        for (size_t i = 0; i < stmts.size(); i++) {
            Stmt* stmt = stmts[i].get();
            if (stmt->kind == StmtKind::RETURN && stmt->expr &&
                stmt->expr->kind == ExprKind::CALL &&
                stmt->expr->call.callee &&
                stmt->expr->call.callee->kind == ExprKind::IDENT &&
                stmt->expr->call.callee->ident == fn.name) {

                // This is a tail call - replace with temp assignments + param reassignment + continue
                std::vector<StmtPtr> replacement;

                // First: evaluate all arguments into temp variables
                for (size_t p = 0; p < fn.params.size() && p < stmt->expr->call.args.size(); p++) {
                    auto assign = std::make_unique<Expr>();
                    assign->kind = ExprKind::ASSIGN;
                    assign->line = stmt->line;
                    assign->column = stmt->column;
                    assign->assign.target = std::make_unique<Expr>();
                    assign->assign.target->kind = ExprKind::IDENT;
                    assign->assign.target->ident = "__tail_tmp_" + fn.params[p].name;
                    assign->assign.value = clone_expr(stmt->expr->call.args[p]);
                    assign->assign.is_decl = true;
                    assign->assign.has_type = false;
                    assign->assign.assign_op = TokenType::ASSIGN;
                    assign->assign.raise = false;

                    auto assign_stmt = std::make_unique<Stmt>();
                    assign_stmt->kind = StmtKind::EXPR;
                    assign_stmt->expr = std::move(assign);
                    replacement.push_back(std::move(assign_stmt));
                }

                // Second: assign temp values to params
                for (size_t p = 0; p < fn.params.size() && p < stmt->expr->call.args.size(); p++) {
                    auto assign = std::make_unique<Expr>();
                    assign->kind = ExprKind::ASSIGN;
                    assign->line = stmt->line;
                    assign->column = stmt->column;
                    assign->assign.target = std::make_unique<Expr>();
                    assign->assign.target->kind = ExprKind::IDENT;
                    assign->assign.target->ident = fn.params[p].name;
                    assign->assign.value = std::make_unique<Expr>();
                    assign->assign.value->kind = ExprKind::IDENT;
                    assign->assign.value->ident = "__tail_tmp_" + fn.params[p].name;
                    assign->assign.is_decl = false;
                    assign->assign.has_type = false;
                    assign->assign.assign_op = TokenType::ASSIGN;
                    assign->assign.raise = false;

                    auto assign_stmt = std::make_unique<Stmt>();
                    assign_stmt->kind = StmtKind::EXPR;
                    assign_stmt->expr = std::move(assign);
                    replacement.push_back(std::move(assign_stmt));
                }

                // Add continue statement
                auto cont = std::make_unique<Stmt>();
                cont->kind = StmtKind::CONTINUE;
                replacement.push_back(std::move(cont));

                stmts[i] = std::make_unique<Stmt>();
                stmts[i]->kind = StmtKind::BLOCK;
                stmts[i]->block.stmts = std::move(replacement);

            } else if (stmt->kind == StmtKind::IF) {
                // Recurse into if branches
                for (auto& branch : stmt->if_stmt.branches) {
                    self(self, branch.body);
                }
                self(self, stmt->if_stmt.else_body);
            } else if (stmt->kind == StmtKind::BLOCK) {
                self(self, stmt->block.stmts);
            }
        }
    };

    transform_returns(transform_returns, while_stmt->while_stmt.body);

    new_body.push_back(std::move(while_stmt));
    fn.body = std::move(new_body);
}

bool Sema::is_error_or_interface_type(const std::string& type_name) {
    return type_name == "error" || interfaces_.count(type_name) > 0;
}

int Sema::get_fn_return_count(const std::string& fn_name, Program& program) {
    for (auto& decl : program.decls) {
        if (decl.kind == DeclKind::FN && decl.fn.name == fn_name) {
            return static_cast<int>(decl.fn.return_types.size());
        }
    }
    return 0;
}

void Sema::check_raise_syntax(Program& program) {
    // Rules (only for functions returning error or interface):
    // 1. Single error-returning call + raise must be standalone (no err :=)
    // 2. Multi-return (T, error) + raise must use binding form (err := foo() raise)

    for (auto& decl : program.decls) {
        if (decl.kind != DeclKind::FN) continue;

        for (auto& stmt : decl.fn.body) {
            if (stmt->kind != StmtKind::EXPR || !stmt->expr) continue;
            if (stmt->expr->kind != ExprKind::CALL) continue;

            // Check for standalone raise on a call expression
            if (!stmt->raise) continue;

            Expr* call_expr = stmt->expr.get();
            if (call_expr->kind != ExprKind::CALL) continue;

            // Extract callee function name
            std::string callee_name;
            if (call_expr->call.callee && call_expr->call.callee->kind == ExprKind::IDENT) {
                callee_name = call_expr->call.callee->ident;
            }

            if (callee_name.empty()) continue;

            // Only enforce for interface-returning functions
            if (interface_returning_fns_.find(callee_name) == interface_returning_fns_.end()) continue;

            // Check if this function returns multiple types including error
            int return_count = get_fn_return_count(callee_name, program);
            if (return_count > 1) {
                // Multi-return: must use binding form
                error("raise on multi-return function '" + callee_name + "' must use binding form (e.g., x := "
                      + callee_name + "() raise)",
                      stmt->line, stmt->column);
            }
        }

        for (auto& stmt : decl.fn.body) {
            if (stmt->kind != StmtKind::EXPR || !stmt->expr) continue;
            if (stmt->expr->kind != ExprKind::ASSIGN) continue;

            auto& assign = stmt->expr->assign;
            if (!assign.raise) continue;

            // Check for binding raise: x := foo() raise
            if (!assign.value || assign.value->kind != ExprKind::CALL) continue;

            Expr* call_expr = assign.value.get();
            if (call_expr->kind != ExprKind::CALL) continue;

            // Extract callee function name
            std::string callee_name;
            if (call_expr->call.callee && call_expr->call.callee->kind == ExprKind::IDENT) {
                callee_name = call_expr->call.callee->ident;
            }

            if (callee_name.empty()) continue;

            // Only enforce for interface-returning functions
            if (interface_returning_fns_.find(callee_name) == interface_returning_fns_.end()) continue;

            // Check if this function returns single error type
            int return_count = get_fn_return_count(callee_name, program);
            if (return_count == 1) {
                // Single error return: must use standalone form
                error("raise on single-error-return function '" + callee_name + "' must be standalone (e.g., "
                      + callee_name + "() raise)",
                      stmt->line, stmt->column);
            }
        }
    }
}

} // namespace binar
