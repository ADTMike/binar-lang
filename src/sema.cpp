#include "sema.h"
#include <functional>

namespace binar {

Sema::Sema(const CompilerConfig& cfg) : config_(cfg) {}

bool Sema::analyze(Program& program) {
    // First pass: collect type declarations and free functions
    for (auto& decl : program.decls) {
        if (decl.kind == DeclKind::TYPE) {
            TypeInfoFull info;
            info.name = decl.type_decl.name;
            for (auto& field : decl.type_decl.fields) {
                info.fields.push_back({field.name, ""});
            }
            types_[decl.type_decl.name] = info;
        }
    }

    for (auto& decl : program.decls) {
        if (decl.kind == DeclKind::FN) {
            FnInfo fi;
            fi.name = decl.fn.name;
            fi.is_method = false;
            if (!decl.fn.params.empty()) {
                std::string first = decl.fn.params[0].type ? resolve_type(*decl.fn.params[0].type) : "";
                if (types_.count(first) > 0) {
                    fi.is_method = true;
                }
            }
            for (auto& p : decl.fn.params) {
                fi.param_types.push_back(p.type ? resolve_type(*p.type) : "");
            }
            for (auto& rt : decl.fn.return_types) {
                fi.return_types.push_back(rt ? resolve_type(*rt) : "");
            }
            functions_[decl.fn.name].push_back(fi);
        }
    }

    // Second pass: check declarations
    for (auto& decl : program.decls) {
        check_decl(decl);
    }

    // Validate return types: max 2, if 2 then second must be error
    for (auto& decl : program.decls) {
        if (decl.kind != DeclKind::FN) continue;
        if (decl.fn.return_types.size() > 2) {
            error("function '" + decl.fn.name + "' has too many return types (max 2)",
                  decl.line, decl.column);
        } else if (decl.fn.return_types.size() == 2) {
            std::string second = decl.fn.return_types[1]->name;
            if (second != "error") {
                error("second return type must be 'error', got '" + second + "'",
                      decl.line, decl.column);
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
        case DeclKind::CONSTANT:
            check_constant_decl(decl.constant_decl, decl.line, decl.column);
            break;
        case DeclKind::GLOBAL_VAR: {
            auto& gvd = decl.global_var_decl;
            if (gvd.type) {
                resolve_type(*gvd.type);
            }
            if (gvd.value) {
                check_expr(*gvd.value);
            }
            break;
        }
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

    // Check body
    for (auto& stmt : fn.body) {
        check_stmt(*stmt);
    }

    // Check structural type methods
    check_structural_methods(fn);

    // Exit scope
    scopes_.pop_back();
}

void Sema::check_structural_methods(FnDecl& fn) {
    // Find all ~TypeName parameters
    std::map<std::string, std::string> structural_params;  // param_name -> type_name
    for (auto& param : fn.params) {
        if (!param.type) continue;
        if (param.type->kind == TypeKind::STRUCTURAL) {
            structural_params[param.name] = param.type->name;
        } else if (param.type->kind == TypeKind::POINTER &&
                   param.type->inner &&
                   param.type->inner->kind == TypeKind::STRUCTURAL) {
            structural_params[param.name] = param.type->inner->name;
        }
    }

    if (structural_params.empty()) return;

    // Scan body for method calls on structural parameters
    for (auto& stmt : fn.body) {
        if (stmt->kind != StmtKind::EXPR || !stmt->expr) continue;
        if (stmt->expr->kind != ExprKind::CALL) continue;

        auto& call = stmt->expr->call;
        if (!call.callee || call.callee->kind != ExprKind::DOT_ACCESS) continue;

        auto& dot = call.callee->dot;
        if (!dot.object || dot.object->kind != ExprKind::IDENT) continue;

        std::string param_name = dot.object->ident;
        auto it = structural_params.find(param_name);
        if (it == structural_params.end()) continue;

        std::string type_name = it->second;
        std::string method_name = dot.field;

        // Check if there's a free function with this method name and the type as first param
        bool found = false;
        auto fn_it = functions_.find(method_name);
        if (fn_it != functions_.end()) {
            for (auto& fi : fn_it->second) {
                if (fi.param_types.size() >= 1) {
                    std::string first = fi.param_types[0];
                    if (first == type_name || first == "*" + type_name) {
                        found = true;
                        break;
                    }
                }
            }
        }

        if (!found) {
            error("type '" + type_name + "' has no method '" + method_name + "'",
                  stmt->line, stmt->column);
        }
    }
}

void Sema::check_type_decl(TypeDecl& td, int line, int column) {
    // Type declarations are mostly checked in the first pass
}

void Sema::check_constant_decl(ConstantDecl& cd, int line, int column) {
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
        case StmtKind::PASS:
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
        case ExprKind::LEN:
            check_expr(*expr.len.arg);
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
        case TypeKind::STRUCTURAL:
            if (types_.find(type.name) == types_.end()) {
                error("unknown type '" + type.name + "' in structural type '~" + type.name + "'",
                      type.line, type.column);
            }
            return "~" + type.name;
        default:
            return "unknown";
    }
}

void Sema::error(const std::string& msg, int line, int column) {
    errors_.push_back({msg, line, column});
}

bool Sema::fn_returns_error(const std::string& fn_name, Program& program) {
    for (auto& decl : program.decls) {
        if (decl.kind == DeclKind::FN && decl.fn.name == fn_name) {
            for (auto& rt : decl.fn.return_types) {
                if (rt->name == "error") return true;
            }
            return false;
        }
    }
    return false;
}

void Sema::check_raise_syntax(Program& program) {
    // Rule: standalone raise on a call to a function returning single error is OK.
    //       standalone raise on a multi-return function returning error is an error
    //       (must capture the other values explicitly).

    for (auto& decl : program.decls) {
        if (decl.kind != DeclKind::FN) continue;

        for (auto& stmt : decl.fn.body) {
            if (stmt->kind != StmtKind::EXPR || !stmt->expr) continue;
            if (stmt->expr->kind != ExprKind::CALL) continue;

            if (!stmt->raise) continue;

            Expr* call_expr = stmt->expr.get();
            if (call_expr->kind != ExprKind::CALL) continue;

            std::string callee_name;
            if (call_expr->call.callee && call_expr->call.callee->kind == ExprKind::IDENT) {
                callee_name = call_expr->call.callee->ident;
            }

            if (callee_name.empty()) continue;

            if (!fn_returns_error(callee_name, program)) continue;

            // Check if this function returns multiple types including error
            for (auto& d : program.decls) {
                if (d.kind == DeclKind::FN && d.fn.name == callee_name) {
                    if (d.fn.return_types.size() > 1) {
                        error("raise on multi-return function '" + callee_name + "' must capture other values explicitly",
                              stmt->line, stmt->column);
                    }
                    break;
                }
            }
        }
    }
}

} // namespace binar
