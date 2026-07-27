#include "codegen.h"
#include "config.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Constants.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/LegacyPassManager.h"
#include <iostream>
#include <unordered_map>
#include <set>
#include <filesystem>

namespace binar {

class CodegenImpl {
public:
    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<llvm::Module> module_;
    std::unique_ptr<llvm::IRBuilder<>> builder_;
    llvm::Function* current_fn_;
    std::unordered_map<std::string, llvm::AllocaInst*> named_values_;
    std::unordered_map<std::string, llvm::Value*> const_values_;
    std::unordered_map<std::string, llvm::StructType*> struct_types_;
    std::unordered_map<std::string, std::vector<std::string>> struct_fields_;
    std::unordered_map<std::string, llvm::StructType*> named_struct_types_;
    std::unordered_map<llvm::Value*, llvm::StructType*> value_struct_type_;
    struct ImportEntry {
        std::string package_path;
        std::string file;
    };
    std::map<std::string, ImportEntry> import_names_;

    struct PendingFile {
        std::string path;
        std::string source;
        Program program;
        bool codegen_done = false;
    };
    std::map<std::string, std::map<std::string, PendingFile>> pending_files_;

    std::string current_package_;
    std::string current_file_;

    // Structural type names registry (from ~TypeName params)
    std::set<std::string> structural_names_;

    // Tagged union infrastructure
    llvm::StructType* error_tu_type_ = nullptr;
    std::map<std::string, llvm::StructType*> structural_tu_types_;

    struct TuImpl {
        std::string type_name;
        int tag;
        llvm::StructType* struct_type;
        uint64_t struct_size;
    };
    std::vector<TuImpl> error_impls_;
    std::map<std::string, std::vector<TuImpl>> structural_impls_reg_;

    // Structural method registry: structural_name -> method signatures
    struct StructuralMethodInfo {
        std::string name;
        std::string param_type;
        bool is_pointer;
        std::string return_type;
    };
    std::map<std::string, std::vector<StructuralMethodInfo>> structural_methods_;

    // Structural parameter tracking (for method dispatch inside functions)
    std::map<std::string, std::string> structural_param_types_;  // param_name -> structural_name

    struct LoopContext {
        llvm::BasicBlock* break_target;
        llvm::BasicBlock* continue_target;
    };
    std::vector<LoopContext> loop_stack_;

    std::vector<Expr*> deferred_calls_;

    bool current_stmt_raise_ = false;

    // ==================== Generics Infrastructure ====================
    // Generic function registry: stores FnDecls that have type_params
    std::map<std::string, FnDecl> generic_fn_decls_;
    // Generic type registry: stores TypeDecls that have type_params
    std::map<std::string, TypeDecl> generic_type_decls_;
    // Monomorphization cache for functions: "Max__int" -> Function*
    std::map<std::string, llvm::Function*> monomorphized_fns_;
    // Monomorphization cache for types: "Pair__int__string" -> StructType*
    std::map<std::string, llvm::StructType*> monomorphized_types_;
    // Reverse map: monomorphized StructType* -> vector of type args
    std::map<llvm::StructType*, std::vector<llvm::Type*>> type_struct_to_args_;
    // Active type substitution map during monomorphization body codegen
    std::map<std::string, llvm::Type*> current_type_subst_;

    // Helper: compute mangled name for generic instantiation
    std::string mangle_generic(const std::string& base_name,
                               const std::vector<llvm::Type*>& type_args) {
        std::string result = base_name;
        for (auto* t : type_args) {
            result += "__";
            if (t->isIntegerTy(64)) result += "int";
            else if (t->isIntegerTy(1)) result += "bool";
            else if (t->isDoubleTy()) result += "float";
            else if (t->isPointerTy()) result += "ptr";
            else if (t->isStructTy()) {
                result += llvm::cast<llvm::StructType>(t)->getStructName().str();
            } else {
                result += "T";
            }
        }
        return result;
    }

    // Helper: infer type args from function call arguments
    bool infer_type_args(FnDecl& fn_decl,
                         const std::vector<llvm::Type*>& arg_types,
                         std::vector<llvm::Type*>& resolved_type_args) {
        // Build param_name -> type_param_index mapping
        std::map<std::string, size_t> param_to_tp;
        for (size_t i = 0; i < fn_decl.type_params.size(); i++) {
            param_to_tp[fn_decl.type_params[i]] = i;
        }
        resolved_type_args.resize(fn_decl.type_params.size(), nullptr);

        for (size_t i = 0; i < fn_decl.params.size() && i < arg_types.size(); i++) {
            auto& param_type = fn_decl.params[i].type;
            if (param_type->kind == TypeKind::TYPE_PARAM) {
                auto it = param_to_tp.find(param_type->name);
                if (it != param_to_tp.end()) {
                    resolved_type_args[it->second] = arg_types[i];
                }
            } else if (param_type->kind == TypeKind::POINTER &&
                       param_type->inner && param_type->inner->kind == TypeKind::TYPE_PARAM) {
                auto it = param_to_tp.find(param_type->inner->name);
                if (it != param_to_tp.end()) {
                    resolved_type_args[it->second] = llvm::PointerType::get(*context_, 0);
                }
            } else if (param_type->kind == TypeKind::NAMED &&
                       !param_type->type_args.empty()) {
                // Handle generic struct params like Pair[T]
                llvm::Type* arg_ty = arg_types[i];
                llvm::StructType* arg_struct = nullptr;
                if (arg_ty->isStructTy()) {
                    arg_struct = llvm::cast<llvm::StructType>(arg_ty);
                }
                if (arg_struct) {
                    auto args_it = type_struct_to_args_.find(arg_struct);
                    if (args_it != type_struct_to_args_.end()) {
                        auto& concrete_args = args_it->second;
                        // Map type_args of the param type to concrete args
                        for (size_t ta = 0; ta < param_type->type_args.size() && ta < concrete_args.size(); ta++) {
                            auto& ta_type = param_type->type_args[ta];
                            if (ta_type->kind == TypeKind::TYPE_PARAM) {
                                auto tp_it = param_to_tp.find(ta_type->name);
                                if (tp_it != param_to_tp.end()) {
                                    resolved_type_args[tp_it->second] = concrete_args[ta];
                                }
                            }
                        }
                    }
                }
            }
        }

        // Check all type params were resolved
        for (size_t i = 0; i < resolved_type_args.size(); i++) {
            if (!resolved_type_args[i]) return false;
        }
        return true;
    }

    // Helper: convert explicit type annotation to LLVM type
    llvm::Type* resolve_type_arg(TypeAnnotation& ta) {
        if (ta.kind == TypeKind::TYPE_PARAM) {
            auto it = current_type_subst_.find(ta.name);
            if (it != current_type_subst_.end()) return it->second;
        }
        return resolve_type(ta);
    }

    llvm::Function* monomorphize_fn(const std::string& fn_name,
                                     std::vector<llvm::Type*>& type_args);
    llvm::StructType* monomorphize_type(const std::string& type_name,
                                         std::vector<llvm::Type*>& type_args);

    std::vector<std::string> errors_;

    llvm::StructType* string_type_ = nullptr;

    void error(const std::string& msg) {
        errors_.push_back(msg);
    }

    bool has_errors() const { return !errors_.empty(); }

    CompilerConfig config_;

    explicit CodegenImpl(const CompilerConfig& cfg = CompilerConfig::default_config())
        : current_fn_(nullptr), config_(cfg) {
        context_ = std::make_unique<llvm::LLVMContext>();
        module_ = std::make_unique<llvm::Module>("binar", *context_);
        builder_ = std::make_unique<llvm::IRBuilder<>>(*context_);

        if (config_.target_init.x86) {
            LLVMInitializeX86TargetInfo();
            LLVMInitializeX86Target();
            LLVMInitializeX86TargetMC();
            LLVMInitializeX86AsmParser();
            LLVMInitializeX86AsmPrinter();
        }

        string_type_ = llvm::StructType::create(*context_, "binar.string");
        string_type_->setBody({llvm::PointerType::get(*context_, 0),
                               llvm::IntegerType::get(*context_, config_.types.int_width)});
        struct_types_["string"] = string_type_;
        struct_types_["binar.string"] = string_type_;
        struct_fields_["string"] = {"ptr", "len"};
        struct_fields_["binar.string"] = {"ptr", "len"};
    }

    void gen_decl(Decl& decl);
    void gen_fn_decl(FnDecl& fn);
    void gen_type_decl(TypeDecl& td);
    void gen_stmt(Stmt& stmt);
    bool generate_lazy(const std::string& package, const std::string& file);
    llvm::Value* gen_expr(Expr& expr);
    llvm::Type* resolve_type(TypeAnnotation& type);
    llvm::Type* resolve_type_by_name(const std::string& name);
    void gen_entry_point();
    llvm::StructType* find_struct_type(const std::string& name);
    std::string extract_type_name(TypeAnnotation* type);
    void emit_raise_check(llvm::Value* result);
    void emit_deferred();
    void emit_exit_code(llvm::Value* code);

    // Tagged union infrastructure
    void collect_tu_info(Program& program);
    void discover_structural_implementations(Program& program);
    void discover_error_types(Program& program);
    void create_tu_types();
    llvm::StructType* create_tu_type(const std::string& name, uint64_t max_buf_size);
    llvm::Value* wrap_in_tu(llvm::Value* concrete_alloca, llvm::StructType* concrete_type,
                             llvm::StructType* tu_type, int tag);
    llvm::Value* ensure_in_tu(llvm::Value* val, llvm::Type* expected_type);
    bool is_tu_type(llvm::Type* ty);
    int find_error_tag(const std::string& type_name);
    int find_structural_tag(const std::string& structural_name, const std::string& type_name);
    uint64_t compute_struct_size(llvm::StructType* sty);
    llvm::Value* gen_structural_dispatch(const std::string& structural_name, const std::string& method_name,
                                    llvm::Value* tu_val, std::vector<llvm::Value*>& args,
                                    llvm::Type* result_type);
};

Codegen::Codegen(const CompilerConfig& cfg) : impl_(std::make_unique<CodegenImpl>(cfg)) {}
Codegen::~Codegen() = default;

bool Codegen::generate(Program& program, const std::string& filename) {
    impl_->collect_tu_info(program);
    for (auto& decl : program.decls) {
        impl_->gen_decl(decl);
    }
    impl_->gen_entry_point();
    bool ok = !llvm::verifyModule(*impl_->module_, &llvm::errs());
    if (!impl_->errors_.empty()) {
        ok = false;
        for (auto& err : impl_->errors_) {
            std::cerr << "error: " << err << std::endl;
        }
    }
    return ok;
}

bool Codegen::generate_imported(Program& program, const std::string& filename) {
    for (auto& decl : program.decls) {
        if (decl.kind != DeclKind::IMPORT) {
            impl_->gen_decl(decl);
        }
    }
    return true;
}

void Codegen::register_pending_file(const std::string& package_name,
                                      const std::string& file_basename,
                                      const std::string& path,
                                      const std::string& source,
                                      Program program) {
    CodegenImpl::PendingFile pf;
    pf.path = path;
    pf.source = source;
    pf.program = std::move(program);
    pf.codegen_done = false;
    impl_->pending_files_[package_name][file_basename] = std::move(pf);
}

bool Codegen::emit_object(const std::string& output) {
    std::string target_triple = impl_->config_.target_triple.empty()
        ? llvm::sys::getDefaultTargetTriple()
        : impl_->config_.target_triple;
    impl_->module_->setTargetTriple(llvm::Triple(target_triple));

    std::string error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(target_triple, error);
    if (!target) {
        llvm::errs() << error;
        return false;
    }

    llvm::TargetOptions options;
    auto* target_machine = target->createTargetMachine(
        llvm::Triple(target_triple), impl_->config_.cpu, "", options, llvm::Reloc::PIC_);

    impl_->module_->setDataLayout(target_machine->createDataLayout());

    std::string outname = output.empty() ? "output.o" : output;
    std::error_code ec;
    llvm::raw_fd_ostream dest(outname, ec);
    if (ec) {
        llvm::errs() << "Could not open file: " << ec.message();
        return false;
    }

    llvm::legacy::PassManager pass;
    if (target_machine->addPassesToEmitFile(pass, dest, nullptr,
        llvm::CodeGenFileType::ObjectFile)) {
        llvm::errs() << "TargetMachine can't emit a file of this type";
        return false;
    }

    pass.run(*impl_->module_);
    dest.flush();
    return true;
}

bool Codegen::emit_ir(const std::string& output) {
    if (output.empty()) {
        impl_->module_->print(llvm::errs(), nullptr);
    } else {
        std::error_code ec;
        llvm::raw_fd_ostream dest(output, ec);
        if (ec) {
            llvm::errs() << "Could not open file: " << ec.message();
            return false;
        }
        impl_->module_->print(dest, nullptr);
    }
    return true;
}

// ==================== Tagged Union Infrastructure ====================

uint64_t CodegenImpl::compute_struct_size(llvm::StructType* sty) {
    uint64_t size = 0;
    for (unsigned i = 0; i < sty->getNumElements(); i++) {
        llvm::Type* elem = sty->getElementType(i);
        uint64_t elem_size = 0;
        if (elem->isIntegerTy(64) || elem->isDoubleTy()) elem_size = 8;
        else if (elem->isIntegerTy(32)) elem_size = 4;
        else if (elem->isIntegerTy(8) || elem->isIntegerTy(1)) elem_size = 1;
        else if (elem->isPointerTy()) elem_size = 8;
        else if (elem->isStructTy()) elem_size = compute_struct_size(llvm::cast<llvm::StructType>(elem));
        else elem_size = 8;
        size = (size + 7) & ~7ULL;
        size += elem_size;
    }
    size = (size + 7) & ~7ULL;
    return size;
}

llvm::StructType* CodegenImpl::create_tu_type(const std::string& name, uint64_t max_buf_size) {
    if (max_buf_size == 0) max_buf_size = 1;
    llvm::Type* tag_type = llvm::Type::getInt32Ty(*context_);
    llvm::Type* buffer_type = llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), max_buf_size);
    llvm::StructType* tu_type = llvm::StructType::create(*context_, name);
    tu_type->setBody({tag_type, buffer_type});
    return tu_type;
}

bool CodegenImpl::is_tu_type(llvm::Type* ty) {
    if (ty == error_tu_type_) return true;
    for (auto& [name, tt] : structural_tu_types_) {
        if (ty == tt) return true;
    }
    return false;
}

int CodegenImpl::find_error_tag(const std::string& type_name) {
    for (auto& impl : error_impls_) {
        if (impl.type_name == type_name) return impl.tag;
    }
    return -1;
}

int CodegenImpl::find_structural_tag(const std::string& structural_name, const std::string& type_name) {
    auto it = structural_impls_reg_.find(structural_name);
    if (it == structural_impls_reg_.end()) return -1;
    for (auto& impl : it->second) {
        if (impl.type_name == type_name) return impl.tag;
    }
    return -1;
}

void CodegenImpl::discover_structural_implementations(Program& program) {
    for (auto& [structural_name, methods] : structural_methods_) {
        std::vector<TuImpl> impls;
        int tag = 1;  // tag 0 reserved for nil

        // Scan free functions: find types whose methods match the structural interface
        for (auto& decl : program.decls) {
            if (decl.kind != DeclKind::FN) continue;
            if (decl.fn.params.empty()) continue;
            // Skip functions whose first param is itself a structural type
            if (decl.fn.params[0].type->kind == TypeKind::STRUCTURAL) continue;
            std::string first_type = extract_type_name(decl.fn.params[0].type.get());
            if (first_type.empty()) continue;
            // Strip pointer prefix for lookup
            if (first_type.size() > 1 && first_type[0] == '*') {
                first_type = first_type.substr(1);
            }

            bool already_added = false;
            for (auto& impl : impls) {
                if (impl.type_name == first_type) { already_added = true; break; }
            }
            if (already_added) continue;

            for (auto& pm : methods) {
                if (decl.fn.name == pm.name && decl.fn.params.size() >= 1) {
                    auto sit = struct_types_.find(first_type);
                    if (sit != struct_types_.end()) {
                        uint64_t sz = compute_struct_size(sit->second);
                        impls.push_back({first_type, tag++, sit->second, sz});
                    }
                    break;
                }
            }
        }
        structural_impls_reg_[structural_name] = std::move(impls);
    }
}

void CodegenImpl::discover_error_types(Program& program) {
    int tag = 1;  // tag 0 is reserved for nil
    for (auto& decl : program.decls) {
        if (decl.kind != DeclKind::FN) continue;
        bool returns_error = false;
        for (auto& rt : decl.fn.return_types) {
            if (rt->name == "error") { returns_error = true; break; }
        }
        if (!returns_error) continue;

        for (auto& stmt : decl.fn.body) {
            if (stmt->kind == StmtKind::RETURN) {
                auto check_expr = [&](ExprPtr& e) {
                    if (e && e->kind == ExprKind::STRUCT_LITERAL) {
                        std::string type_name = e->struct_literal.type_name;
                        bool found = false;
                        for (auto& impl : error_impls_) {
                            if (impl.type_name == type_name) { found = true; break; }
                        }
                        if (!found) {
                            auto sit = struct_types_.find(type_name);
                            if (sit != struct_types_.end()) {
                                uint64_t sz = compute_struct_size(sit->second);
                                error_impls_.push_back({type_name, tag++, sit->second, sz});
                            }
                        }
                    }
                };
                if (!stmt->return_values.empty()) {
                    for (auto& rv : stmt->return_values) check_expr(rv);
                } else if (stmt->expr) {
                    check_expr(stmt->expr);
                }
            } else if (stmt->kind == StmtKind::IF) {
                for (auto& branch : stmt->if_stmt.branches) {
                    for (auto& s : branch.body) {
                        if (s->kind == StmtKind::RETURN) {
                            auto check_e = [&](ExprPtr& e) {
                                if (e && e->kind == ExprKind::STRUCT_LITERAL) {
                                    std::string tn = e->struct_literal.type_name;
                                    bool found = false;
                                    for (auto& im : error_impls_) {
                                        if (im.type_name == tn) { found = true; break; }
                                    }
                                    if (!found) {
                                        auto si = struct_types_.find(tn);
                                        if (si != struct_types_.end()) {
                                            uint64_t sz = compute_struct_size(si->second);
                                            error_impls_.push_back({tn, tag++, si->second, sz});
                                        }
                                    }
                                }
                            };
                            if (!s->return_values.empty()) {
                                for (auto& rv : s->return_values) check_e(rv);
                            } else if (s->expr) {
                                check_e(s->expr);
                            }
                        }
                    }
                }
            }
        }
    }
}

void CodegenImpl::create_tu_types() {
    uint64_t max_error_size = 0;
    for (auto& impl : error_impls_) {
        if (impl.struct_size > max_error_size) max_error_size = impl.struct_size;
    }
    error_tu_type_ = create_tu_type("error.tu", max_error_size > 0 ? max_error_size : 8);

    for (auto& [structural_name, impls] : structural_impls_reg_) {
        uint64_t max_size = 0;
        for (auto& impl : impls) {
            if (impl.struct_size > max_size) max_size = impl.struct_size;
        }
        structural_tu_types_[structural_name] = create_tu_type(structural_name + ".tu", max_size > 0 ? max_size : 8);
    }
}

void CodegenImpl::collect_tu_info(Program& program) {
    // First: scan all functions for ~TypeName parameters to register structural type names
    for (auto& decl : program.decls) {
        if (decl.kind != DeclKind::FN) continue;
        for (auto& param : decl.fn.params) {
            if (param.type->kind == TypeKind::STRUCTURAL) {
                structural_names_.insert(param.type->name);
            } else if (param.type->kind == TypeKind::POINTER &&
                       param.type->inner &&
                       param.type->inner->kind == TypeKind::STRUCTURAL) {
                structural_names_.insert(param.type->inner->name);
            }
        }
    }

    // Second: for each structural name, scan function bodies to discover required methods
    // We look for method calls on structural parameters: param.Method(...) or (*param).Method(...)
    for (auto& decl : program.decls) {
        if (decl.kind != DeclKind::FN) continue;
        for (auto& param : decl.fn.params) {
            std::string structural_name;
            if (param.type->kind == TypeKind::STRUCTURAL) {
                structural_name = param.type->name;
            } else if (param.type->kind == TypeKind::POINTER &&
                       param.type->inner &&
                       param.type->inner->kind == TypeKind::STRUCTURAL) {
                structural_name = param.type->inner->name;
            }
            if (structural_name.empty()) continue;

            // Scan body for method calls on this parameter
            for (auto& stmt : decl.fn.body) {
                if (stmt->kind != StmtKind::EXPR || !stmt->expr) continue;
                if (stmt->expr->kind != ExprKind::CALL) continue;
                auto& call = stmt->expr->call;
                if (!call.callee || call.callee->kind != ExprKind::DOT_ACCESS) continue;
                auto& dot = call.callee->dot;
                if (!dot.object || dot.object->kind != ExprKind::IDENT) continue;
                if (dot.object->ident != param.name) continue;

                std::string method_name = dot.field;

                // Check if method already registered
                bool found = false;
                for (auto& existing : structural_methods_[structural_name]) {
                    if (existing.name == method_name) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    StructuralMethodInfo mi;
                    mi.name = method_name;
                    mi.is_pointer = false;
                    mi.param_type = "";
                    structural_methods_[structural_name].push_back(mi);
                }
            }
        }
    }

    for (auto& decl : program.decls) {
        if (decl.kind == DeclKind::TYPE) {
            gen_type_decl(decl.type_decl);
        }
    }

    discover_structural_implementations(program);
    discover_error_types(program);
    create_tu_types();
}

llvm::Value* CodegenImpl::wrap_in_tu(llvm::Value* concrete_alloca, llvm::StructType* concrete_type,
                                       llvm::StructType* tu_type, int tag) {
    llvm::Value* tu_alloca = builder_->CreateAlloca(tu_type, nullptr, "tu.tmp");

    llvm::Value* tag_ptr = builder_->CreateGEP(tu_type, tu_alloca,
        {llvm::ConstantInt::get(*context_, llvm::APInt(32, 0)),
         llvm::ConstantInt::get(*context_, llvm::APInt(32, 0))}, "tu.tag.ptr");
    builder_->CreateStore(llvm::ConstantInt::get(*context_, llvm::APInt(32, tag)), tag_ptr);

    llvm::Value* buf_ptr = builder_->CreateGEP(tu_type, tu_alloca,
        {llvm::ConstantInt::get(*context_, llvm::APInt(32, 0)),
         llvm::ConstantInt::get(*context_, llvm::APInt(32, 1))}, "tu.buf.ptr");

    llvm::Type* i8ptr = llvm::PointerType::get(*context_, 0);
    llvm::Value* src = builder_->CreateBitCast(concrete_alloca, i8ptr, "tu.src");
    llvm::Value* dst = builder_->CreateBitCast(buf_ptr, i8ptr, "tu.dst");
    const llvm::DataLayout& dl = module_->getDataLayout();
    uint64_t size = dl.getTypeStoreSize(concrete_type);
    builder_->CreateMemCpy(dst, llvm::MaybeAlign(1), src, llvm::MaybeAlign(1), size);

    return builder_->CreateLoad(tu_type, tu_alloca, "tu.val");
}

llvm::Value* CodegenImpl::ensure_in_tu(llvm::Value* val, llvm::Type* expected_type) {
    if (!val) {
        return llvm::Constant::getNullValue(expected_type);
    }
    llvm::Type* val_type = val->getType();
    if (val_type == expected_type) return val;

    if (expected_type->isPointerTy() && val_type->isPointerTy()) return val;

    llvm::StructType* tu_type = llvm::cast<llvm::StructType>(expected_type);

    if (val_type->isPointerTy() && llvm::isa<llvm::ConstantPointerNull>(val)) {
        return llvm::Constant::getNullValue(tu_type);
    }

    llvm::StructType* concrete_type = nullptr;
    std::string concrete_name;

    if (val_type->isStructTy() && val_type != tu_type) {
        concrete_type = llvm::cast<llvm::StructType>(val_type);
        concrete_name = strip_struct_prefix(concrete_type->getStructName().str());
    } else if (val_type->isPointerTy()) {
        auto vsti = value_struct_type_.find(val);
        if (vsti != value_struct_type_.end() && vsti->second != tu_type) {
            concrete_type = vsti->second;
            concrete_name = strip_struct_prefix(concrete_type->getStructName().str());
            val = builder_->CreateLoad(concrete_type, val, "tu.deref");
        } else if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(val)) {
            llvm::Type* pointee = alloca->getAllocatedType();
            if (pointee->isStructTy() && pointee != tu_type) {
                concrete_type = llvm::cast<llvm::StructType>(pointee);
                concrete_name = strip_struct_prefix(concrete_type->getStructName().str());
                val = builder_->CreateLoad(concrete_type, alloca, "tu.deref");
            }
        }
    }

    if (concrete_type) {
        int tag = 0;
        if (tu_type == error_tu_type_) {
            tag = find_error_tag(concrete_name);
            if (tag < 0) tag = 0;
        } else {
            for (auto& [sname, stt] : structural_tu_types_) {
                if (stt == tu_type) {
                    tag = find_structural_tag(sname, concrete_name);
                    if (tag < 0) tag = 0;
                    break;
                }
            }
        }

        llvm::AllocaInst* concrete_alloca = builder_->CreateAlloca(concrete_type, nullptr, "wrap.tmp");
        builder_->CreateStore(val, concrete_alloca);
        return wrap_in_tu(concrete_alloca, concrete_type, tu_type, tag);
    }

    return llvm::Constant::getNullValue(tu_type);
}

llvm::Value* CodegenImpl::gen_structural_dispatch(const std::string& structural_name,
                                             const std::string& method_name,
                                             llvm::Value* tu_val,
                                             std::vector<llvm::Value*>& args,
                                             llvm::Type* result_type) {
    auto structural_it = structural_tu_types_.find(structural_name);
    if (structural_it == structural_tu_types_.end()) {
        error("unknown structural type '" + structural_name + "'");
        return llvm::ConstantInt::get(*context_, llvm::APInt(64, 0));
    }
    llvm::StructType* tu_type = structural_it->second;

    auto impl_it = structural_impls_reg_.find(structural_name);
    if (impl_it == structural_impls_reg_.end() || impl_it->second.empty()) {
        error("no implementations for structural type '" + structural_name + "'");
        return llvm::ConstantInt::get(*context_, llvm::APInt(64, 0));
    }

    llvm::Function* caller_fn = builder_->GetInsertBlock()->getParent();
    llvm::Value* tu_alloca = builder_->CreateAlloca(tu_type, nullptr, "dispatch.tu");
    builder_->CreateStore(tu_val, tu_alloca);

    llvm::Value* tag_ptr = builder_->CreateGEP(tu_type, tu_alloca,
        {llvm::ConstantInt::get(*context_, llvm::APInt(32, 0)),
         llvm::ConstantInt::get(*context_, llvm::APInt(32, 0))}, "dispatch.tag.ptr");
    llvm::Value* tag = builder_->CreateLoad(llvm::Type::getInt32Ty(*context_), tag_ptr, "dispatch.tag");

    llvm::BasicBlock* default_bb = llvm::BasicBlock::Create(*context_, "dispatch.default", caller_fn);
    llvm::BasicBlock* merge_bb = llvm::BasicBlock::Create(*context_, "dispatch.merge", caller_fn);

    llvm::SwitchInst* sw = builder_->CreateSwitch(tag, default_bb, impl_it->second.size());

    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> results;

    for (auto& impl : impl_it->second) {
        llvm::BasicBlock* case_bb = llvm::BasicBlock::Create(*context_, "dispatch.case." + impl.type_name, caller_fn);
        sw->addCase(llvm::ConstantInt::get(*context_, llvm::APInt(32, impl.tag)), case_bb);

        builder_->SetInsertPoint(case_bb);

        llvm::Value* buf_ptr = builder_->CreateGEP(tu_type, tu_alloca,
            {llvm::ConstantInt::get(*context_, llvm::APInt(32, 0)),
             llvm::ConstantInt::get(*context_, llvm::APInt(32, 1))}, "dispatch.buf");

        llvm::Value* concrete_ptr;
        if (args.size() > 0 && args[0]->getType()->isPointerTy()) {
            concrete_ptr = builder_->CreateBitCast(buf_ptr, args[0]->getType(), "dispatch.concrete.ptr");
        } else {
            concrete_ptr = buf_ptr;
        }

        if (impl.struct_type->getNumElements() > 0 || impl.struct_type->getStructName().str().find("void") != std::string::npos) {

            std::string mangled = method_name + "__" + impl.type_name;
            llvm::Function* method_fn = module_->getFunction(mangled);
            if (!method_fn) {
                method_fn = module_->getFunction(method_name);
            }
            if (!method_fn) {
                for (auto& decl_fn : pending_files_) {
                    for (auto& [fname, pf] : decl_fn.second) {
                        for (auto& d : pf.program.decls) {
                            if (d.kind == DeclKind::FN && d.fn.name == method_name) {
                                if (!d.fn.params.empty()) {
                                    std::string ptype = extract_type_name(d.fn.params[0].type.get());
                                    if (!ptype.empty() && ptype[0] == '*') ptype = ptype.substr(1);
                                    if (ptype == impl.type_name) {
                                        std::string lazy_name = fname + "." + method_name;
                                        method_fn = module_->getFunction(lazy_name);
                                        break;
                                    }
                                }
                            }
                        }
                        if (method_fn) break;
                    }
                    if (method_fn) break;
                }
            }
            if (!method_fn && !current_file_.empty()) {
                method_fn = module_->getFunction(current_file_ + "." + mangled);
                if (!method_fn) {
                    method_fn = module_->getFunction(current_file_ + "." + method_name);
                }
            }

            llvm::Value* concrete_arg;
            bool method_expects_ptr = method_fn && method_fn->arg_size() > 0 &&
                method_fn->getArg(0)->getType()->isPointerTy();
            if (method_expects_ptr) {
                concrete_ptr = builder_->CreateBitCast(buf_ptr, method_fn->getArg(0)->getType(), "dispatch.concrete.ptr");
                concrete_arg = concrete_ptr;
            } else {
                concrete_arg = builder_->CreateLoad(impl.struct_type, concrete_ptr, "dispatch.concrete");
            }

            if (method_fn) {
                std::vector<llvm::Value*> call_args;
                call_args.push_back(concrete_arg);
                for (size_t i = 1; i < args.size(); i++) {
                    call_args.push_back(args[i]);
                }

                llvm::Value* call_result;
                if (method_fn->getReturnType()->isVoidTy()) {
                    call_result = builder_->CreateCall(method_fn, call_args);
                } else {
                    call_result = builder_->CreateCall(method_fn, call_args, "dispatch.call");
                }
                results.push_back({call_result, case_bb});
            } else {
                results.push_back({llvm::ConstantInt::get(*context_, llvm::APInt(64, 0)), case_bb});
            }
        } else {
            results.push_back({llvm::ConstantInt::get(*context_, llvm::APInt(64, 0)), case_bb});
        }

        builder_->CreateBr(merge_bb);
    }

    builder_->SetInsertPoint(default_bb);
    builder_->CreateUnreachable();

    builder_->SetInsertPoint(merge_bb);

    if (result_type->isVoidTy()) {
        return nullptr;
    }

    llvm::PHINode* phi = builder_->CreatePHI(result_type, results.size(), "dispatch.result");
    for (auto& [r, bb] : results) {
        phi->addIncoming(r, bb);
    }
    return phi;
}

// ==================== Monomorphization ====================

llvm::Function* CodegenImpl::monomorphize_fn(const std::string& fn_name,
                                              std::vector<llvm::Type*>& type_args) {
    // Build mangled name
    std::string mangled = mangle_generic(fn_name, type_args);

    // Check cache
    auto cache_it = monomorphized_fns_.find(mangled);
    if (cache_it != monomorphized_fns_.end()) {
        return cache_it->second;
    }

    // Look up generic decl
    auto decl_it = generic_fn_decls_.find(fn_name);
    if (decl_it == generic_fn_decls_.end()) {
        error("unknown generic function '" + fn_name + "'");
        return nullptr;
    }

    // We need a mutable copy to set up substitution — but FnDecl has StmtPtrs (unique_ptr)
    // Use a thread-local static workaround: store the decl reference and the subst map
    FnDecl& fn_decl = decl_it->second;

    // Build substitution map
    auto saved_subst = current_type_subst_;
    for (size_t i = 0; i < fn_decl.type_params.size() && i < type_args.size(); i++) {
        current_type_subst_[fn_decl.type_params[i]] = type_args[i];
    }

    // Resolve parameter types with substitution
    std::vector<llvm::Type*> param_types;
    for (auto& param : fn_decl.params) {
        param_types.push_back(resolve_type(*param.type));
    }

    // Resolve return types
    llvm::Type* return_type;
    bool is_multi_return = fn_decl.return_types.size() > 1;
    if (is_multi_return) {
        std::vector<llvm::Type*> ret_field_types;
        for (auto& rt : fn_decl.return_types) {
            ret_field_types.push_back(resolve_type(*rt));
        }
        return_type = llvm::StructType::get(*context_, ret_field_types, true);
    } else if (fn_decl.return_types.empty()) {
        return_type = llvm::Type::getVoidTy(*context_);
    } else {
        return_type = resolve_type(*fn_decl.return_types[0]);
    }

    // Save state BEFORE creating the new function
    auto saved_fn = current_fn_;
    auto saved_named = named_values_;
    auto saved_insert_block = builder_->GetInsertBlock();
    auto saved_struct_types = named_struct_types_;

    llvm::FunctionType* fn_type = llvm::FunctionType::get(return_type, param_types, false);
    std::string llvm_fn_name = current_file_.empty() ? mangled : current_file_ + "." + mangled;
    llvm::Function* func = llvm::Function::Create(
        fn_type, llvm::Function::ExternalLinkage, llvm_fn_name, module_.get());

    // Create entry block and codegen body
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context_, "entry", func);
    builder_->SetInsertPoint(entry);
    current_fn_ = func;

    // Map params to allocas
    for (size_t i = 0; i < fn_decl.params.size(); i++) {
        llvm::AllocaInst* alloca = builder_->CreateAlloca(param_types[i], nullptr, fn_decl.params[i].name);
        builder_->CreateStore(func->getArg(i), alloca);
        named_values_[fn_decl.params[i].name] = alloca;
    }

    // Codegen body
    for (auto& stmt : fn_decl.body) {
        gen_stmt(*stmt);
    }

    // Add return if body doesn't end with one
    if (builder_->GetInsertBlock()->getTerminator() == nullptr) {
        if (return_type->isVoidTy()) {
            builder_->CreateRetVoid();
        } else {
            builder_->CreateRet(llvm::Constant::getNullValue(return_type));
        }
    }

    // Restore state
    current_fn_ = saved_fn;
    named_values_ = saved_named;
    current_type_subst_ = saved_subst;
    named_struct_types_ = saved_struct_types;
    if (saved_insert_block) {
        builder_->SetInsertPoint(saved_insert_block);
    }

    monomorphized_fns_[mangled] = func;
    return func;
}

llvm::StructType* CodegenImpl::monomorphize_type(const std::string& type_name,
                                                  std::vector<llvm::Type*>& type_args) {
    std::string mangled = mangle_generic(type_name, type_args);

    auto cache_it = monomorphized_types_.find(mangled);
    if (cache_it != monomorphized_types_.end()) {
        return cache_it->second;
    }

    auto decl_it = generic_type_decls_.find(type_name);
    if (decl_it == generic_type_decls_.end()) {
        error("unknown generic type '" + type_name + "'");
        return nullptr;
    }

    TypeDecl& type_decl = decl_it->second;

    // Build substitution map
    auto saved_subst = current_type_subst_;
    for (size_t i = 0; i < type_decl.type_params.size() && i < type_args.size(); i++) {
        current_type_subst_[type_decl.type_params[i]] = type_args[i];
    }

    // Build field types
    std::vector<llvm::Type*> field_types;
    std::vector<std::string> field_names;
    for (auto& field : type_decl.fields) {
        field_types.push_back(resolve_type(*field.type));
        field_names.push_back(field.name);
    }

    current_type_subst_ = saved_subst;

    if (field_types.empty()) {
        field_types.push_back(llvm::Type::getInt8Ty(*context_));
    }

    llvm::StructType* stype = llvm::StructType::get(*context_, field_types, false);
    stype->setName(mangled);

    struct_types_[mangled] = stype;
    struct_fields_[mangled] = field_names;

    monomorphized_types_[mangled] = stype;
    type_struct_to_args_[stype] = type_args;
    return stype;
}

// ==================== CodegenImpl ====================

void CodegenImpl::gen_decl(Decl& decl) {
    switch (decl.kind) {
        case DeclKind::FN: gen_fn_decl(decl.fn); break;
        case DeclKind::TYPE: break;
        case DeclKind::CONSTANT: {
            auto& cd = decl.constant_decl;
            if (cd.value) {
                llvm::Value* val = gen_expr(*cd.value);
                if (current_fn_) {
                    llvm::AllocaInst* alloca = builder_->CreateAlloca(val->getType(), nullptr, cd.name);
                    builder_->CreateStore(val, alloca);
                    named_values_[cd.name] = alloca;
                } else {
                    const_values_[cd.name] = val;
                }
            }
            break;
        }
        case DeclKind::GLOBAL_VAR: {
            auto& gvd = decl.global_var_decl;
            if (gvd.value) {
                llvm::Value* val = gen_expr(*gvd.value);
                if (val) {
                    const_values_[gvd.name] = val;
                }
            }
            break;
        }
        case DeclKind::IMPORT: {
            for (auto& binding : decl.import_block.bindings) {
                ImportEntry entry;
                entry.package_path = binding.package_path;
                entry.file = binding.name;
                import_names_[binding.name] = entry;
            }
            break;
        }
    }
}

void CodegenImpl::gen_fn_decl(FnDecl& fn) {
    if (!fn.has_body) return;

    // Generic functions: store in registry, don't codegen immediately
    if (!fn.type_params.empty()) {
        generic_fn_decls_[fn.name] = std::move(fn);
        return;
    }

    structural_param_types_.clear();

    std::vector<llvm::Type*> param_types;
    for (auto& param : fn.params) {
        param_types.push_back(resolve_type(*param.type));
    }

    llvm::Type* return_type;
    bool is_multi_return = fn.return_types.size() > 1;

    if (is_multi_return) {
        std::vector<llvm::Type*> ret_field_types;
        for (auto& rt : fn.return_types) {
            ret_field_types.push_back(resolve_type(*rt));
        }
        return_type = llvm::StructType::get(*context_, ret_field_types, true);
    } else if (fn.return_types.empty()) {
        return_type = llvm::Type::getVoidTy(*context_);
    } else {
        return_type = resolve_type(*fn.return_types[0]);
    }

    llvm::FunctionType* fn_type = llvm::FunctionType::get(return_type, param_types, false);
    std::string llvm_fn_name = current_file_.empty() ? fn.name : current_file_ + "." + fn.name;
    llvm::Function* func = llvm::Function::Create(
        fn_type, llvm::Function::ExternalLinkage, llvm_fn_name, module_.get());

    unsigned idx = 0;
    for (auto& param : fn.params) {
        func->getArg(idx++)->setName(param.name);
    }

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context_, "entry", func);
    builder_->SetInsertPoint(entry);
    current_fn_ = func;

    deferred_calls_.clear();
    named_values_.clear();
    idx = 0;
    for (auto& param : fn.params) {
        llvm::Type* ptype = resolve_type(*param.type);
        llvm::AllocaInst* alloca = builder_->CreateAlloca(ptype, nullptr, param.name);
        builder_->CreateStore(func->getArg(idx), alloca);
        named_values_[param.name] = alloca;

        std::string ptype_name = extract_type_name(param.type.get());
        if (!ptype_name.empty() && structural_names_.count(ptype_name) > 0) {
            structural_param_types_[param.name] = ptype_name;
        }

        idx++;
    }

    idx = 0;
    for (auto& param : fn.params) {
        if (param.type->kind == TypeKind::NAMED) {
            auto sit = struct_types_.find(param.type->name);
            if (sit != struct_types_.end()) {
                named_struct_types_[param.name] = sit->second;
            }
        } else if (param.type->kind == TypeKind::POINTER &&
                   param.type->inner &&
                   param.type->inner->kind == TypeKind::NAMED) {
            auto sit = struct_types_.find(param.type->inner->name);
            if (sit != struct_types_.end()) {
                named_struct_types_[param.name] = sit->second;
            }
        }
    }

    for (auto& stmt : fn.body) {
        gen_stmt(*stmt);
    }

    if (!builder_->GetInsertBlock()->getTerminator()) {
        emit_deferred();
        if (return_type->isVoidTy()) {
            builder_->CreateRetVoid();
        } else if (is_multi_return) {
            builder_->CreateRet(llvm::Constant::getNullValue(return_type));
        } else if (is_tu_type(return_type)) {
            builder_->CreateRet(llvm::Constant::getNullValue(return_type));
        } else if (return_type->isPointerTy()) {
            builder_->CreateRet(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(return_type)));
        } else {
            builder_->CreateRet(llvm::ConstantInt::get(*context_, llvm::APInt(64, 0)));
        }
    }

    current_fn_ = nullptr;
    llvm::verifyFunction(*func, &llvm::errs());
}

void CodegenImpl::gen_type_decl(TypeDecl& td) {
    // Generic types: store in registry, don't codegen immediately
    if (!td.type_params.empty()) {
        generic_type_decls_[td.name] = std::move(td);
        return;
    }

    std::string qualified_name = "struct." + td.name;

    llvm::StructType* struct_type = llvm::StructType::create(*context_, qualified_name);

    std::vector<llvm::Type*> field_types;
    std::vector<std::string> field_names;
    for (auto& field : td.fields) {
        field_types.push_back(resolve_type(*field.type));
        field_names.push_back(field.name);
    }

    struct_type->setBody(field_types);
    struct_types_[td.name] = struct_type;
    struct_fields_[td.name] = field_names;
}

llvm::StructType* CodegenImpl::find_struct_type(const std::string& name) {
    auto sit = named_struct_types_.find(name);
    if (sit != named_struct_types_.end()) return sit->second;
    auto tit = struct_types_.find(name);
    if (tit != struct_types_.end()) return tit->second;
    return nullptr;
}

bool CodegenImpl::generate_lazy(const std::string& package, const std::string& file) {
    auto pit = pending_files_.find(package);
    if (pit == pending_files_.end()) return false;
    auto fit = pit->second.find(file);
    if (fit == pit->second.end()) return false;
    if (fit->second.codegen_done) return true;

    fit->second.codegen_done = true;

    std::string saved_package = current_package_;
    std::string saved_file = current_file_;
    llvm::Function* saved_fn = current_fn_;
    current_package_ = package;
    current_file_ = file;

    for (auto& decl : fit->second.program.decls) {
        gen_decl(decl);
    }

    current_package_ = saved_package;
    current_file_ = saved_file;
    current_fn_ = saved_fn;
    return true;
}

std::string CodegenImpl::extract_type_name(TypeAnnotation* type) {
    if (!type) return "";
    switch (type->kind) {
        case TypeKind::NAMED:
            return type->name;
        case TypeKind::POINTER:
            if (type->inner) return "*" + extract_type_name(type->inner.get());
            return "*";
        case TypeKind::TYPE_PARAM:
            return type->name;
        default:
            return "";
    }
}

void CodegenImpl::gen_entry_point() {
    if (module_->getFunction(config_.entry_point.symbol.c_str())) return;

    llvm::Function* main_fn = module_->getFunction("main");
    if (!main_fn) return;

    llvm::FunctionType* start_type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(*context_), false);
    llvm::Function* start_fn = llvm::Function::Create(
        start_type, llvm::Function::ExternalLinkage,
        config_.entry_point.symbol, module_.get());

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context_, "entry", start_fn);
    builder_->SetInsertPoint(entry);

    if (main_fn->getReturnType()->isVoidTy()) {
        builder_->CreateCall(main_fn);
        emit_exit_code(llvm::ConstantInt::get(*context_, llvm::APInt(64, 0)));
    } else if (is_tu_type(main_fn->getReturnType())) {
        llvm::Value* ret = builder_->CreateCall(main_fn);
        llvm::Value* tag = builder_->CreateExtractValue(ret, {0}, "main.tag");
        llvm::Value* is_nil = builder_->CreateICmpEQ(tag,
            llvm::ConstantInt::get(*context_, llvm::APInt(32, 0)), "main.is_nil");

        llvm::BasicBlock* exit0_bb = llvm::BasicBlock::Create(*context_, "exit.0", start_fn);
        llvm::BasicBlock* exit1_bb = llvm::BasicBlock::Create(*context_, "exit.1", start_fn);

        builder_->CreateCondBr(is_nil, exit0_bb, exit1_bb);

        builder_->SetInsertPoint(exit0_bb);
        emit_exit_code(llvm::ConstantInt::get(*context_, llvm::APInt(64, 0)));

        builder_->SetInsertPoint(exit1_bb);
        emit_exit_code(llvm::ConstantInt::get(*context_, llvm::APInt(64, 1)));
    } else {
        llvm::Value* ret = builder_->CreateCall(main_fn);
        llvm::Value* is_nil = builder_->CreateICmpEQ(ret,
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ret->getType())),
            "main.is_nil");

        llvm::BasicBlock* exit0_bb = llvm::BasicBlock::Create(*context_, "exit.0", start_fn);
        llvm::BasicBlock* exit1_bb = llvm::BasicBlock::Create(*context_, "exit.1", start_fn);

        builder_->CreateCondBr(is_nil, exit0_bb, exit1_bb);

        builder_->SetInsertPoint(exit0_bb);
        emit_exit_code(llvm::ConstantInt::get(*context_, llvm::APInt(64, 0)));

        builder_->SetInsertPoint(exit1_bb);
        emit_exit_code(llvm::ConstantInt::get(*context_, llvm::APInt(64, 1)));
    }
}

void CodegenImpl::emit_raise_check(llvm::Value* result) {
    llvm::Type* result_type = result->getType();
    llvm::Value* err_val = nullptr;

    if (result_type->isStructTy()) {
        llvm::StructType* sty = llvm::cast<llvm::StructType>(result_type);
        if (sty == error_tu_type_) {
            err_val = result;
        } else {
            unsigned n = sty->getNumElements();
            llvm::Type* last_type = sty->getElementType(n - 1);
            if (last_type == error_tu_type_ || (last_type->isStructTy() && is_tu_type(last_type))) {
                err_val = builder_->CreateExtractValue(result, {n - 1}, "raise.err");
            } else if (last_type->isPointerTy()) {
                err_val = builder_->CreateExtractValue(result, {n - 1}, "raise.err");
            } else if (last_type->isIntegerTy(64)) {
                err_val = builder_->CreateExtractValue(result, {n - 1}, "raise.err");
            }
        }
    }

    if (!err_val) return;

    llvm::Value* is_err;
    if (err_val->getType()->isStructTy() && is_tu_type(err_val->getType())) {
        llvm::Value* tag = builder_->CreateExtractValue(err_val, {0}, "raise.tag");
        is_err = builder_->CreateICmpNE(tag,
            llvm::ConstantInt::get(*context_, llvm::APInt(32, 0)), "raise.cond");
    } else if (err_val->getType()->isPointerTy()) {
        is_err = builder_->CreateICmpNE(err_val,
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(err_val->getType())), "raise.cond");
    } else {
        is_err = builder_->CreateICmpNE(err_val,
            llvm::ConstantInt::get(*context_, llvm::APInt(64, 0)), "raise.cond");
    }

    llvm::Function* fn = builder_->GetInsertBlock()->getParent();
    llvm::BasicBlock* err_bb = llvm::BasicBlock::Create(*context_, "raise.ret", fn);
    llvm::BasicBlock* cont_bb = llvm::BasicBlock::Create(*context_, "raise.cont", fn);

    builder_->CreateCondBr(is_err, err_bb, cont_bb);

    builder_->SetInsertPoint(err_bb);
    emit_deferred();
    llvm::Type* ret_type = fn->getReturnType();
    if (ret_type->isVoidTy()) {
        emit_exit_code(llvm::ConstantInt::get(*context_, llvm::APInt(64, 1)));
    } else if (is_tu_type(ret_type)) {
        builder_->CreateRet(err_val);
    } else if (ret_type->isStructTy()) {
        llvm::StructType* ret_sty = llvm::cast<llvm::StructType>(ret_type);
        llvm::Value* ret_val = llvm::Constant::getNullValue(ret_type);
        unsigned ret_n = ret_sty->getNumElements();
        ret_val = builder_->CreateInsertValue(ret_val, err_val, {ret_n - 1});
        builder_->CreateRet(ret_val);
    } else {
        builder_->CreateRet(err_val);
    }

    builder_->SetInsertPoint(cont_bb);
}

void CodegenImpl::emit_deferred() {
    for (int i = deferred_calls_.size() - 1; i >= 0; i--) {
        gen_expr(*deferred_calls_[i]);
    }
}

void CodegenImpl::emit_exit_code(llvm::Value* code) {
    if (!code->getType()->isIntegerTy(64)) {
        code = builder_->CreateIntCast(code, llvm::Type::getInt64Ty(*context_), true);
    }
    auto it = config_.syscall.calls.find("exit");
    if (it == config_.syscall.calls.end()) return;
    std::string asm_str = "movq $$" + std::to_string(it->second.number) + ", %rax\n\t"
                        + "movq $0, %rdi\n\t"
                        + config_.syscall.syscall_insn;
    llvm::FunctionType* exit_type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(*context_),
        {llvm::Type::getInt64Ty(*context_)}, false);
    llvm::InlineAsm* exit_asm = llvm::InlineAsm::get(
        exit_type, asm_str, "r," + it->second.clobbers, true);
    builder_->CreateCall(exit_asm, {code});
    builder_->CreateUnreachable();
}

void CodegenImpl::gen_stmt(Stmt& stmt) {
    switch (stmt.kind) {
        case StmtKind::EXPR:
            if (stmt.expr) {
                if (stmt.expr->kind == ExprKind::ASSIGN) {
                    auto& assign = stmt.expr->assign;

                    if (assign.is_decl) {
                        std::string name = assign.target->ident;
                        llvm::Type* ty = nullptr;
                        llvm::Value* val = nullptr;

                        if (assign.has_type) {
                            ty = resolve_type_by_name(assign.type_name);
                            if (assign.value) {
                                val = gen_expr(*assign.value);
                                if (val && val->getType()->isPointerTy() && ty->isStructTy()) {
                                    ty = val->getType();
                                }
                            } else {
                                val = llvm::Constant::getNullValue(ty);
                            }
                        } else {
                            current_stmt_raise_ = assign.raise;
                            val = gen_expr(*assign.value);
                            current_stmt_raise_ = false;
                            if (val && assign.raise) {
                                if (val->getType()->isStructTy()) {
                                    emit_raise_check(val);
                                    if (is_tu_type(val->getType())) {
                                        val = nullptr;
                                    } else {
                                        val = builder_->CreateExtractValue(val, {0});
                                    }
                                }
                            }
                            ty = val ? val->getType() : llvm::Type::getInt64Ty(*context_);
                        }

                        if (val) {
                            llvm::AllocaInst* alloca = builder_->CreateAlloca(ty, nullptr, name);
                            builder_->CreateStore(val, alloca);
                            named_values_[name] = alloca;

                            if (assign.has_type) {
                                auto sit = struct_types_.find(assign.type_name);
                                if (sit != struct_types_.end()) {
                                    named_struct_types_[name] = sit->second;
                                }
                            }

                            if (assign.value && assign.value->kind == ExprKind::STRUCT_LITERAL) {
                                auto it = struct_types_.find(assign.value->struct_literal.type_name);
                                if (it != struct_types_.end()) {
                                    named_struct_types_[name] = it->second;
                                }
                                if (val) {
                                    auto vsti = value_struct_type_.find(val);
                                    if (vsti != value_struct_type_.end()) {
                                        named_struct_types_[name] = vsti->second;
                                    }
                                }
                            } else if (assign.value && assign.value->kind == ExprKind::IDENT) {
                                auto sit = named_struct_types_.find(assign.value->ident);
                                if (sit != named_struct_types_.end()) {
                                    named_struct_types_[name] = sit->second;
                                }
                            }
                        } else {
                            llvm::BasicBlock* unreachable = llvm::BasicBlock::Create(*context_, "unreachable.after.raise", current_fn_);
                            builder_->SetInsertPoint(unreachable);
                        }
                    } else {
                        current_stmt_raise_ = assign.raise;
                        llvm::Value* val = gen_expr(*assign.value);
                        current_stmt_raise_ = false;
                        if (val && assign.raise) {
                            if (val->getType()->isStructTy()) {
                                emit_raise_check(val);
                                val = nullptr;
                            }
                        }
                        if (!val) {
                            llvm::BasicBlock* unreachable = llvm::BasicBlock::Create(*context_, "unreachable.after.raise", current_fn_);
                            builder_->SetInsertPoint(unreachable);
                        } else if (assign.target->kind == ExprKind::UNARY && assign.target->unary.op == UnOp::DEREF) {
                            llvm::Value* ptr = nullptr;
                            if (assign.target->unary.operand->kind == ExprKind::IDENT) {
                                auto it = named_values_.find(assign.target->unary.operand->ident);
                                if (it != named_values_.end()) {
                                    ptr = builder_->CreateLoad(
                                        it->second->getAllocatedType(), it->second,
                                        assign.target->unary.operand->ident + ".ptr");
                                }
                            }
                            if (!ptr) {
                                ptr = gen_expr(*assign.target->unary.operand);
                            }
                            builder_->CreateStore(val, ptr);
                        } else if (assign.target->kind == ExprKind::DOT_ACCESS) {
                            std::string obj_name = "";
                            if (assign.target->dot.object->kind == ExprKind::IDENT) {
                                obj_name = assign.target->dot.object->ident;
                            }
                            llvm::StructType* stype = nullptr;
                            llvm::Value* obj = nullptr;

                            if (!obj_name.empty()) {
                                stype = find_struct_type(obj_name);
                            }

                            if (!obj_name.empty()) {
                                auto ait = named_values_.find(obj_name);
                                if (ait != named_values_.end()) {
                                    llvm::Type* alloc_type = ait->second->getAllocatedType();
                                    if (alloc_type->isStructTy()) {
                                        stype = llvm::cast<llvm::StructType>(alloc_type);
                                        obj = ait->second;
                                    } else if (alloc_type->isPointerTy()) {
                                        if (stype) {
                                            obj = builder_->CreateLoad(alloc_type, ait->second, obj_name);
                                        }
                                    }
                                }
                            }
                            if (!obj) {
                                obj = gen_expr(*assign.target->dot.object);
                            }

                            if (stype) {
                                std::string struct_name = strip_struct_prefix(stype->getStructName().str());
                                auto fit = struct_fields_.find(struct_name);
                                if (fit != struct_fields_.end()) {
                                    int field_idx = -1;
                                    for (size_t i = 0; i < fit->second.size(); i++) {
                                        if (fit->second[i] == assign.target->dot.field) {
                                            field_idx = i;
                                            break;
                                        }
                                    }
                                    if (field_idx >= 0) {
                                        llvm::Value* field_ptr = builder_->CreateGEP(stype, obj,
                                            {llvm::ConstantInt::get(*context_, llvm::APInt(32, 0)),
                                             llvm::ConstantInt::get(*context_, llvm::APInt(32, field_idx))},
                                            assign.target->dot.field);
                                        builder_->CreateStore(val, field_ptr);
                                    } else {
                                        std::cerr << "error: unknown field '" << assign.target->dot.field << "'" << std::endl;
                                    }
                                }
                            }
                        } else {
                            std::string name = assign.target->ident;
                            auto it = named_values_.find(name);
                            if (it != named_values_.end()) {
                                if (assign.assign_op != TokenType::ASSIGN) {
                                    llvm::Type* alloc_ty = it->second->getAllocatedType();
                                    llvm::Value* cur = builder_->CreateLoad(alloc_ty, it->second, name + ".old");
                                    switch (assign.assign_op) {
                                        case TokenType::PLUS_ASSIGN:
                                            val = builder_->CreateAdd(cur, val, name + ".add"); break;
                                        case TokenType::MINUS_ASSIGN:
                                            val = builder_->CreateSub(cur, val, name + ".sub"); break;
                                        case TokenType::STAR_ASSIGN:
                                            val = builder_->CreateMul(cur, val, name + ".mul"); break;
                                        case TokenType::SLASH_ASSIGN:
                                            val = builder_->CreateSDiv(cur, val, name + ".div"); break;
                                        case TokenType::PERCENT_ASSIGN:
                                            val = builder_->CreateSRem(cur, val, name + ".rem"); break;
                                        case TokenType::AMP_ASSIGN:
                                            val = builder_->CreateAnd(cur, val, name + ".and"); break;
                                        case TokenType::PIPE_ASSIGN:
                                            val = builder_->CreateOr(cur, val, name + ".or"); break;
                                        case TokenType::CARET_ASSIGN:
                                            val = builder_->CreateXor(cur, val, name + ".xor"); break;
                                        case TokenType::LSHIFT_ASSIGN:
                                            val = builder_->CreateShl(cur, val, name + ".shl"); break;
                                        case TokenType::RSHIFT_ASSIGN:
                                            val = builder_->CreateAShr(cur, val, name + ".shr"); break;
                                        default: break;
                                    }
                                }
                                builder_->CreateStore(val, it->second);
                            } else {
                                std::cerr << "error: undefined variable '" << name << "'" << std::endl;
                            }
                        }
                    }
                } else {
                    current_stmt_raise_ = stmt.raise;
                    llvm::Value* result = gen_expr(*stmt.expr);
                    current_stmt_raise_ = false;
                    if (result && stmt.raise) {
                        if (result->getType()->isStructTy()) {
                            emit_raise_check(result);
                        }
                    }
                    if (!result) {
                    }
                }
            }
            break;

        case StmtKind::RETURN:
            if (!stmt.return_values.empty()) {
                llvm::Type* ret_type = current_fn_->getReturnType();
                llvm::Value* result = llvm::UndefValue::get(ret_type);
                for (size_t i = 0; i < stmt.return_values.size(); i++) {
                    llvm::Value* val = gen_expr(*stmt.return_values[i]);
                    llvm::StructType* sty = llvm::cast<llvm::StructType>(ret_type);
                    llvm::Type* elem_type = sty->getElementType((unsigned)i);

                    if (is_tu_type(elem_type)) {
                        val = ensure_in_tu(val, elem_type);
                    } else if (val->getType()->isPointerTy() && elem_type->isStructTy()) {
                        if (!llvm::isa<llvm::ConstantPointerNull>(val)) {
                            val = builder_->CreateLoad(elem_type, val, "retload");
                        }
                    } else if (val->getType()->isPointerTy() && elem_type->isIntegerTy()) {
                        val = builder_->CreatePtrToInt(val, elem_type);
                    }
                    result = builder_->CreateInsertValue(result, val, (unsigned)i);
                }
                emit_deferred();
                builder_->CreateRet(result);
            } else if (stmt.expr) {
                llvm::Value* val = gen_expr(*stmt.expr);
                llvm::Type* ret_type = current_fn_->getReturnType();

                if (is_tu_type(ret_type) && val) {
                    val = ensure_in_tu(val, ret_type);
                } else if (val && val->getType()->isPointerTy() && ret_type->isStructTy() &&
                    !llvm::isa<llvm::ConstantPointerNull>(val) &&
                    !ret_type->isPointerTy()) {
                    val = builder_->CreateLoad(ret_type, val, "retload");
                }
                emit_deferred();
                builder_->CreateRet(val);
            } else {
                emit_deferred();
                llvm::Type* ret_type = current_fn_->getReturnType();
                if (ret_type->isVoidTy()) {
                    builder_->CreateRetVoid();
                } else if (is_tu_type(ret_type)) {
                    builder_->CreateRet(llvm::Constant::getNullValue(ret_type));
                } else {
                    builder_->CreateRet(llvm::ConstantInt::get(*context_, llvm::APInt(64, 0)));
                }
            }
            break;

        case StmtKind::IF: {
            for (size_t i = 0; i < stmt.if_stmt.branches.size(); i++) {
                auto& branch = stmt.if_stmt.branches[i];
                llvm::Value* cond = gen_expr(*branch.condition);
                cond = builder_->CreateICmpNE(cond,
                    llvm::ConstantInt::getFalse(*context_), "ifcond");

                llvm::Function* fn = builder_->GetInsertBlock()->getParent();
                llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(*context_, "then", fn);
                llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(*context_, "else");
                llvm::BasicBlock* merge_bb = llvm::BasicBlock::Create(*context_, "ifcont");

                builder_->CreateCondBr(cond, then_bb, else_bb);

                builder_->SetInsertPoint(then_bb);
                for (auto& s : branch.body) gen_stmt(*s);
                if (!builder_->GetInsertBlock()->getTerminator())
                    builder_->CreateBr(merge_bb);

                else_bb->insertInto(fn);
                builder_->SetInsertPoint(else_bb);
                if (i == stmt.if_stmt.branches.size() - 1) {
                    for (auto& s : stmt.if_stmt.else_body) gen_stmt(*s);
                }
                if (!builder_->GetInsertBlock()->getTerminator())
                    builder_->CreateBr(merge_bb);

                merge_bb->insertInto(fn);
                builder_->SetInsertPoint(merge_bb);
            }
            break;
        }

        case StmtKind::BLOCK:
            for (auto& s : stmt.block.stmts) gen_stmt(*s);
            break;

        case StmtKind::WHILE: {
            llvm::Function* fn = builder_->GetInsertBlock()->getParent();
            llvm::BasicBlock* cond_bb = llvm::BasicBlock::Create(*context_, "while.cond", fn);
            llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(*context_, "while.body", fn);
            llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(*context_, "while.end", fn);

            loop_stack_.push_back({end_bb, cond_bb});

            builder_->CreateBr(cond_bb);
            builder_->SetInsertPoint(cond_bb);
            llvm::Value* cond = gen_expr(*stmt.while_stmt.condition);
            cond = builder_->CreateICmpNE(cond,
                llvm::ConstantInt::getFalse(*context_), "whilecond");
            builder_->CreateCondBr(cond, body_bb, end_bb);

            builder_->SetInsertPoint(body_bb);
            for (auto& s : stmt.while_stmt.body) gen_stmt(*s);
            if (!builder_->GetInsertBlock()->getTerminator())
                builder_->CreateBr(cond_bb);

            loop_stack_.pop_back();
            builder_->SetInsertPoint(end_bb);
            break;
        }

        case StmtKind::FOR: {
            llvm::Function* fn = builder_->GetInsertBlock()->getParent();
            llvm::BasicBlock* cond_bb = llvm::BasicBlock::Create(*context_, "for.cond", fn);
            llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(*context_, "for.body", fn);
            llvm::BasicBlock* incr_bb = llvm::BasicBlock::Create(*context_, "for.incr", fn);
            llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(*context_, "for.end", fn);

            loop_stack_.push_back({end_bb, incr_bb});

            if (stmt.for_stmt.init) gen_stmt(*stmt.for_stmt.init);
            builder_->CreateBr(cond_bb);

            builder_->SetInsertPoint(cond_bb);
            if (stmt.for_stmt.cond) {
                llvm::Value* cond = gen_expr(*stmt.for_stmt.cond);
                cond = builder_->CreateICmpNE(cond,
                    llvm::ConstantInt::getFalse(*context_), "forcond");
                builder_->CreateCondBr(cond, body_bb, end_bb);
            } else {
                builder_->CreateBr(body_bb);
            }

            builder_->SetInsertPoint(body_bb);
            for (auto& s : stmt.for_stmt.body) gen_stmt(*s);
            if (!builder_->GetInsertBlock()->getTerminator())
                builder_->CreateBr(incr_bb);

            builder_->SetInsertPoint(incr_bb);
            if (stmt.for_stmt.update) gen_expr(*stmt.for_stmt.update);
            builder_->CreateBr(cond_bb);

            loop_stack_.pop_back();
            builder_->SetInsertPoint(end_bb);
            break;
        }

        case StmtKind::FOR_RANGE:
            for (auto& s : stmt.range_stmt.body) gen_stmt(*s);
            break;

        case StmtKind::BREAK:
            if (!loop_stack_.empty()) {
                builder_->CreateBr(loop_stack_.back().break_target);
                llvm::BasicBlock* unreachable = llvm::BasicBlock::Create(*context_, "after.break", current_fn_);
                builder_->SetInsertPoint(unreachable);
            }
            break;
        case StmtKind::PASS:
            if (!loop_stack_.empty()) {
                builder_->CreateBr(loop_stack_.back().continue_target);
                llvm::BasicBlock* unreachable = llvm::BasicBlock::Create(*context_, "after.continue", current_fn_);
                builder_->SetInsertPoint(unreachable);
            }
            break;
        case StmtKind::DEFER:
            if (stmt.defer_stmt.expr) {
                deferred_calls_.push_back(stmt.defer_stmt.expr.get());
            }
            break;
        case StmtKind::SWITCH:
            break;

        case StmtKind::ASM: {
            bool has_operands = !stmt.asm_stmt.outputs.empty() ||
                                !stmt.asm_stmt.inputs.empty() ||
                                !stmt.asm_stmt.clobbers.empty();

            if (!has_operands) {
                llvm::FunctionType* asm_type = llvm::FunctionType::get(
                    llvm::Type::getVoidTy(*context_), false);
                llvm::InlineAsm* inline_asm = llvm::InlineAsm::get(
                    asm_type, stmt.asm_stmt.code, "", true);
                builder_->CreateCall(inline_asm);
            } else {
                std::vector<llvm::Type*> arg_types;
                std::vector<llvm::Value*> arg_vals;
                std::string output_constraints;
                std::string input_constraints;
                std::string clobber_str;

                for (auto& op : stmt.asm_stmt.outputs) {
                    if (op.value) {
                        llvm::Value* val = gen_expr(*op.value);
                        arg_types.push_back(val->getType());
                        arg_vals.push_back(val);
                        if (!output_constraints.empty()) output_constraints += ",";
                        output_constraints += op.constraint;
                    }
                }
                for (auto& op : stmt.asm_stmt.inputs) {
                    if (op.value) {
                        llvm::Value* val = gen_expr(*op.value);
                        arg_types.push_back(val->getType());
                        arg_vals.push_back(val);
                        if (!input_constraints.empty()) input_constraints += ",";
                        input_constraints += op.constraint;
                    }
                }
                for (size_t i = 0; i < stmt.asm_stmt.clobbers.size(); i++) {
                    if (i > 0) clobber_str += ",";
                    clobber_str += "~{" + stmt.asm_stmt.clobbers[i] + "}";
                }

                std::string constraint_str = output_constraints + ";" + input_constraints + ";" + clobber_str;

                llvm::FunctionType* asm_type = llvm::FunctionType::get(
                    llvm::Type::getVoidTy(*context_), arg_types, false);
                llvm::InlineAsm* inline_asm = llvm::InlineAsm::get(
                    asm_type, stmt.asm_stmt.code, constraint_str, true);
                builder_->CreateCall(inline_asm, arg_vals);
            }
            break;
        }

        case StmtKind::MULTI_ASSIGN: {
            llvm::Value* val = gen_expr(*stmt.multi_assign.value);

            if (val->getType()->isStructTy()) {
                llvm::StructType* sty = llvm::cast<llvm::StructType>(val->getType());
                for (size_t i = 0; i < stmt.multi_assign.targets.size() && i < sty->getNumElements(); i++) {
                    llvm::Value* elem = builder_->CreateExtractValue(val, (unsigned)i);
                    std::string name = stmt.multi_assign.targets[i];
                    llvm::AllocaInst* alloca = builder_->CreateAlloca(elem->getType(), nullptr, name);
                    builder_->CreateStore(elem, alloca);
                    named_values_[name] = alloca;
                }
            } else {
                std::cerr << "error: multi-assign requires multi-return function call" << std::endl;
            }
            break;
        }
    }
}

llvm::Value* CodegenImpl::gen_expr(Expr& expr) {
    switch (expr.kind) {
        case ExprKind::INT_LIT:
            return llvm::ConstantInt::get(*context_, llvm::APInt(64, expr.int_val, true));

        case ExprKind::FLOAT_LIT:
            return llvm::ConstantFP::get(*context_, llvm::APFloat(expr.float_val));

        case ExprKind::STRING_LIT: {
            llvm::Value* str_ptr = builder_->CreateGlobalString(expr.string_val);
            llvm::Value* str_len = llvm::ConstantInt::get(*context_,
                llvm::APInt(config_.types.int_width, expr.string_val.size()));
            return llvm::ConstantStruct::get(string_type_,
                {llvm::cast<llvm::Constant>(str_ptr), llvm::cast<llvm::Constant>(str_len)});
        }

        case ExprKind::BOOL_LIT:
            return llvm::ConstantInt::get(*context_, llvm::APInt(1, expr.bool_val ? 1 : 0));

        case ExprKind::NIL_LIT:
            return llvm::ConstantPointerNull::get(llvm::PointerType::get(*context_, 0));

        case ExprKind::IDENT: {
            auto it = named_values_.find(expr.ident);
            if (it != named_values_.end()) {
                llvm::Value* loaded = builder_->CreateLoad(it->second->getAllocatedType(), it->second, expr.ident);
                if (it->second->getAllocatedType()->isPointerTy()) {
                    auto sit = named_struct_types_.find(expr.ident);
                    if (sit != named_struct_types_.end()) {
                        value_struct_type_[loaded] = sit->second;
                    }
                }
                return loaded;
            }
            auto cit = const_values_.find(expr.ident);
            if (cit != const_values_.end()) {
                return cit->second;
            }
            std::cerr << "error: undefined variable '" << expr.ident << "'" << std::endl;
            return llvm::ConstantInt::get(*context_, llvm::APInt(64, 0));
        }

        case ExprKind::BINARY: {
            llvm::Value* left = gen_expr(*expr.binary.left);
            llvm::Value* right = gen_expr(*expr.binary.right);
            switch (expr.binary.op) {
                case BinOp::ADD: {
                    if (left->getType()->isPointerTy() && right->getType()->isIntegerTy()) {
                        return builder_->CreateGEP(
                            llvm::Type::getInt8Ty(*context_), left, right, "gepadd");
                    } else if (right->getType()->isPointerTy() && left->getType()->isIntegerTy()) {
                        return builder_->CreateGEP(
                            llvm::Type::getInt8Ty(*context_), right, left, "gepadd");
                    }
                    return builder_->CreateAdd(left, right, "addtmp");
                }
                case BinOp::SUB: {
                    if (left->getType()->isPointerTy() && right->getType()->isIntegerTy()) {
                        auto neg = builder_->CreateNeg(right, "negidx");
                        return builder_->CreateGEP(
                            llvm::Type::getInt8Ty(*context_), left, neg, "gepsub");
                    } else if (left->getType()->isPointerTy() && right->getType()->isPointerTy()) {
                        return builder_->CreatePtrDiff(
                            llvm::Type::getInt8Ty(*context_), left, right, "ptrdiff");
                    }
                    return builder_->CreateSub(left, right, "subtmp");
                }
                case BinOp::MUL: return builder_->CreateMul(left, right, "multmp");
                case BinOp::DIV: return builder_->CreateSDiv(left, right, "divtmp");
                case BinOp::MOD: return builder_->CreateSRem(left, right, "modtmp");
                case BinOp::EQ: {
                    if (is_tu_type(left->getType()) && right->getType()->isPointerTy()) {
                        llvm::Value* tag = builder_->CreateExtractValue(left, {0}, "tu.tag");
                        return builder_->CreateICmpEQ(tag,
                            llvm::ConstantInt::get(*context_, llvm::APInt(32, 0)), "tu.is_nil");
                    }
                    if (left->getType()->isPointerTy() && is_tu_type(right->getType())) {
                        llvm::Value* tag = builder_->CreateExtractValue(right, {0}, "tu.tag");
                        return builder_->CreateICmpEQ(tag,
                            llvm::ConstantInt::get(*context_, llvm::APInt(32, 0)), "tu.is_nil");
                    }
                    if (left->getType()->isStructTy() && !is_tu_type(left->getType())) {
                        left = builder_->CreateExtractValue(left, 0, "ptr");
                        right = builder_->CreateExtractValue(right, 0, "ptr");
                    }
                    return builder_->CreateICmpEQ(left, right, "eqtmp");
                }
                case BinOp::NEQ: {
                    if (is_tu_type(left->getType()) && right->getType()->isPointerTy()) {
                        llvm::Value* tag = builder_->CreateExtractValue(left, {0}, "tu.tag");
                        return builder_->CreateICmpNE(tag,
                            llvm::ConstantInt::get(*context_, llvm::APInt(32, 0)), "tu.is_nonnil");
                    }
                    if (left->getType()->isPointerTy() && is_tu_type(right->getType())) {
                        llvm::Value* tag = builder_->CreateExtractValue(right, {0}, "tu.tag");
                        return builder_->CreateICmpNE(tag,
                            llvm::ConstantInt::get(*context_, llvm::APInt(32, 0)), "tu.is_nonnil");
                    }
                    if (left->getType()->isStructTy() && !is_tu_type(left->getType())) {
                        left = builder_->CreateExtractValue(left, 0, "ptr");
                        right = builder_->CreateExtractValue(right, 0, "ptr");
                    }
                    return builder_->CreateICmpNE(left, right, "neqtmp");
                }
                case BinOp::LT: return builder_->CreateICmpSLT(left, right, "lttmp");
                case BinOp::GT: return builder_->CreateICmpSGT(left, right, "gttmp");
                case BinOp::LTE: return builder_->CreateICmpSLE(left, right, "letmp");
                case BinOp::GTE: return builder_->CreateICmpSGE(left, right, "getmp");
                case BinOp::AND: return builder_->CreateAnd(left, right, "andtmp");
                case BinOp::OR: return builder_->CreateOr(left, right, "ortmp");
                case BinOp::BIT_AND: return builder_->CreateAnd(left, right, "bandtmp");
                case BinOp::BIT_OR: return builder_->CreateOr(left, right, "bortmp");
                case BinOp::BIT_XOR: return builder_->CreateXor(left, right, "bxortmp");
                case BinOp::LSHIFT: return builder_->CreateShl(left, right, "lshltmp");
                case BinOp::RSHIFT: return builder_->CreateAShr(left, right, "rshltmp");
            }
            return nullptr;
        }

        case ExprKind::UNARY: {
            llvm::Value* operand = gen_expr(*expr.unary.operand);
            switch (expr.unary.op) {
                case UnOp::NEG: return builder_->CreateNeg(operand, "negtmp");
                case UnOp::NOT: {
                    llvm::Value* bool_val = builder_->CreateICmpNE(operand,
                        llvm::ConstantInt::get(operand->getType(), 0), "tobool");
                    return builder_->CreateNot(bool_val, "nottmp");
                }
                case UnOp::BIT_NOT: return builder_->CreateNot(operand, "bitnottmp");
                case UnOp::DEREF: {
                    llvm::Type* inner_type = llvm::Type::getInt64Ty(*context_);
                    return builder_->CreateLoad(inner_type, operand, "deref");
                }
                case UnOp::ADDR_OF: {
                    if (expr.unary.operand->kind == ExprKind::IDENT) {
                        auto it = named_values_.find(expr.unary.operand->ident);
                        if (it != named_values_.end()) {
                            return it->second;
                        }
                    }
                    return operand;
                }
                default: return operand;
            }
        }

        case ExprKind::ASSIGN: {
            llvm::Value* val = gen_expr(*expr.assign.value);
            std::string name = expr.assign.target->ident;
            auto it = named_values_.find(name);
            if (it != named_values_.end()) {
                builder_->CreateStore(val, it->second);
            }
            return val;
        }

        case ExprKind::CALL: {
            std::string fn_name_bare;
            std::string fn_name_mangled;
            llvm::Value* receiver = nullptr;
            bool is_cross_package = false;

            auto is_exported = [](const std::string& s) -> bool {
                return !s.empty() && s[0] >= 'A' && s[0] <= 'Z';
            };

            auto lazy_with_state = [&](const std::string& pkg, const std::string& file) {
                llvm::BasicBlock* prev_block = builder_->GetInsertBlock();
                auto prev_values = named_values_;
                auto prev_struct_types = named_struct_types_;
                llvm::Function* prev_fn = current_fn_;
                generate_lazy(pkg, file);
                current_fn_ = prev_fn;
                if (prev_block) {
                    builder_->SetInsertPoint(prev_block);
                }
                named_values_ = std::move(prev_values);
                named_struct_types_ = std::move(prev_struct_types);
            };

            if (expr.call.callee->kind == ExprKind::IDENT) {
                fn_name_bare = expr.call.callee->ident;
                fn_name_mangled = fn_name_bare;
            } else if (expr.call.callee->kind == ExprKind::DOT_ACCESS) {
                fn_name_bare = expr.call.callee->dot.field;
                fn_name_mangled = fn_name_bare;

                if (expr.call.callee->dot.object->kind == ExprKind::IDENT) {
                    std::string first = expr.call.callee->dot.object->ident;

                    auto it = import_names_.find(first);
                    if (it != import_names_.end()) {
                        auto& entry = it->second;
                        std::string pkg = entry.package_path;
                        if (pkg.empty()) pkg = current_package_;
                        lazy_with_state(pkg, entry.file);
                        fn_name_mangled = entry.file + "." + fn_name_bare;
                        if (!entry.package_path.empty()) {
                            is_cross_package = true;
                        }
                    } else {
                        receiver = gen_expr(*expr.call.callee->dot.object);
                    }
                } else {
                    receiver = gen_expr(*expr.call.callee->dot.object);
                }
            }

            std::vector<llvm::Value*> args;
            for (auto& arg : expr.call.args) {
                args.push_back(gen_expr(*arg));
            }

            // Generic function call detection
            if (expr.call.callee->kind == ExprKind::GENERIC_REF) {
                fn_name_bare = expr.call.callee->generic_ref.name;
                // Resolve explicit type args
                std::vector<llvm::Type*> type_args;
                for (auto& ta : expr.call.callee->generic_ref.type_args) {
                    type_args.push_back(resolve_type(*ta));
                }
                // Build arg types for inference, resolving struct types from named_struct_types_
                std::vector<llvm::Type*> arg_types;
                for (size_t ai = 0; ai < args.size(); ai++) {
                    llvm::Type* ty = args[ai]->getType();
                    if (ai < expr.call.args.size() && expr.call.args[ai]->kind == ExprKind::IDENT) {
                        auto nsit = named_struct_types_.find(expr.call.args[ai]->ident);
                        if (nsit != named_struct_types_.end()) {
                            ty = nsit->second;
                        }
                    }
                    arg_types.push_back(ty);
                }

                // If no explicit type args, try to infer
                if (type_args.empty()) {
                    auto decl_it = generic_fn_decls_.find(fn_name_bare);
                    if (decl_it != generic_fn_decls_.end()) {
                        if (!infer_type_args(decl_it->second, arg_types, type_args)) {
                            error("cannot infer type arguments for '" + fn_name_bare + "'");
                            return llvm::ConstantInt::get(*context_, llvm::APInt(64, 0));
                        }
                    }
                }

                llvm::Function* gen_func = monomorphize_fn(fn_name_bare, type_args);
                if (!gen_func) return llvm::ConstantInt::get(*context_, llvm::APInt(64, 0));

                // Convert args to match function signature
                for (size_t i = 0; i < args.size() && i < gen_func->arg_size(); i++) {
                    llvm::Type* param_type = gen_func->getArg(i)->getType();
                    llvm::Type* arg_type = args[i]->getType();
                    if (param_type != arg_type) {
                        if (param_type->isStructTy() && arg_type->isPointerTy()) {
                            args[i] = builder_->CreateLoad(param_type, args[i], "generic.arg");
                        } else if (is_tu_type(param_type) && arg_type->isStructTy() && arg_type != param_type) {
                            args[i] = ensure_in_tu(args[i], param_type);
                        }
                    }
                }

                if (gen_func->getReturnType()->isVoidTy()) {
                    builder_->CreateCall(gen_func, args);
                    return nullptr;
                }
                return builder_->CreateCall(gen_func, args, "calltmp");
            }

            // Check if callee is IDENT pointing to a generic function (inferred call)
            if (expr.call.callee->kind == ExprKind::IDENT) {
                auto gen_it = generic_fn_decls_.find(fn_name_bare);
                if (gen_it != generic_fn_decls_.end()) {
                    std::vector<llvm::Type*> arg_types;
                    for (size_t ai = 0; ai < args.size(); ai++) {
                        llvm::Type* ty = args[ai]->getType();
                        if (ai < expr.call.args.size() && expr.call.args[ai]->kind == ExprKind::IDENT) {
                            auto nsit = named_struct_types_.find(expr.call.args[ai]->ident);
                            if (nsit != named_struct_types_.end()) {
                                ty = nsit->second;
                            }
                        }
                        arg_types.push_back(ty);
                    }
                    std::vector<llvm::Type*> type_args;
                    if (!infer_type_args(gen_it->second, arg_types, type_args)) {
                        error("cannot infer type arguments for '" + fn_name_bare + "'");
                        return llvm::ConstantInt::get(*context_, llvm::APInt(64, 0));
                    }
                    llvm::Function* gen_func = monomorphize_fn(fn_name_bare, type_args);
                    if (!gen_func) return llvm::ConstantInt::get(*context_, llvm::APInt(64, 0));

                    for (size_t i = 0; i < args.size() && i < gen_func->arg_size(); i++) {
                        llvm::Type* param_type = gen_func->getArg(i)->getType();
                        llvm::Type* arg_type = args[i]->getType();
                        if (param_type != arg_type) {
                            if (param_type->isStructTy() && arg_type->isPointerTy()) {
                                args[i] = builder_->CreateLoad(param_type, args[i], "generic.arg");
                            } else if (is_tu_type(param_type) && arg_type->isStructTy() && arg_type != param_type) {
                                args[i] = ensure_in_tu(args[i], param_type);
                            }
                        }
                    }

                    if (gen_func->getReturnType()->isVoidTy()) {
                        builder_->CreateCall(gen_func, args);
                        return nullptr;
                    }
                    return builder_->CreateCall(gen_func, args, "calltmp");
                }
            }

            if (is_cross_package && !is_exported(fn_name_bare)) {
                error("cannot access unexported function '" + fn_name_bare + "'");
                return llvm::ConstantInt::get(*context_, llvm::APInt(64, 0));
            }

            llvm::Function* func = nullptr;

            if (!fn_name_bare.empty()) {
                func = module_->getFunction(fn_name_mangled);
                if (!func && !fn_name_mangled.empty() && fn_name_mangled == fn_name_bare &&
                    !current_file_.empty()) {
                    func = module_->getFunction(current_file_ + "." + fn_name_mangled);
                }
            } else {
                llvm::Value* callee_val = gen_expr(*expr.call.callee);
                func = llvm::dyn_cast<llvm::Function>(callee_val);
            }

            if (!func) {
                std::cerr << "error: undefined function '" << fn_name_mangled << "'" << std::endl;
                return llvm::ConstantInt::get(*context_, llvm::APInt(64, 0));
            }

            if (receiver && func->arg_size() > 0) {
                llvm::Type* param_type = func->getArg(0)->getType();
                llvm::Type* receiver_type = receiver->getType();

                if (is_tu_type(receiver_type) && receiver_type != param_type) {
                    std::string structural_name;
                    for (auto& [sname, stt] : structural_tu_types_) {
                        if (receiver_type == stt) {
                            structural_name = sname;
                            break;
                        }
                    }
                    if (!structural_name.empty()) {
                        llvm::Type* ret_type = func->getReturnType();
                        return gen_structural_dispatch(structural_name, fn_name_bare, receiver, args, ret_type);
                    }
                }

                if (!receiver_type->isPointerTy() && param_type->isPointerTy()) {
                    llvm::AllocaInst* alloca = builder_->CreateAlloca(receiver_type, nullptr, "methodtmp");
                    builder_->CreateStore(receiver, alloca);
                    receiver = alloca;
                } else if (receiver_type->isPointerTy() && !param_type->isPointerTy()) {
                    receiver = builder_->CreateLoad(param_type, receiver, "deref");
                } else if (receiver_type->isStructTy() && param_type->isStructTy() &&
                           receiver_type != param_type) {
                    if (is_tu_type(param_type)) {
                        receiver = ensure_in_tu(receiver, param_type);
                    }
                }
                args.insert(args.begin(), receiver);
            }

            for (size_t i = 0; i < args.size() && i < func->arg_size(); i++) {
                llvm::Type* param_type = func->getArg(i)->getType();
                llvm::Type* arg_type = args[i]->getType();
                if (param_type->isStructTy() && arg_type->isPointerTy()) {
                    if (is_tu_type(param_type)) {
                        args[i] = ensure_in_tu(args[i], param_type);
                    } else {
                        args[i] = builder_->CreateLoad(param_type, args[i], "structarg");
                    }
                } else if (is_tu_type(param_type) && arg_type->isStructTy() && arg_type != param_type) {
                    args[i] = ensure_in_tu(args[i], param_type);
                }
            }

            if (func->getReturnType()->isVoidTy()) {
                builder_->CreateCall(func, args);
                return nullptr;
            }
            return builder_->CreateCall(func, args, "calltmp");
        }

        case ExprKind::DOT_ACCESS: {
            std::string obj_name = "";
            if (expr.dot.object->kind == ExprKind::IDENT) {
                obj_name = expr.dot.object->ident;
            }
            llvm::StructType* stype = nullptr;
            llvm::Value* obj = nullptr;

            if (!obj_name.empty()) {
                stype = find_struct_type(obj_name);
            }

            if (!obj_name.empty()) {
                auto ait = named_values_.find(obj_name);
                if (ait != named_values_.end()) {
                    llvm::Type* alloc_type = ait->second->getAllocatedType();
                    if (alloc_type->isStructTy()) {
                        stype = llvm::cast<llvm::StructType>(alloc_type);
                        obj = ait->second;
                    } else if (alloc_type->isPointerTy()) {
                        if (stype) {
                            obj = builder_->CreateLoad(alloc_type, ait->second, obj_name);
                        }
                    }
                }
            }
            if (!obj) {
                obj = gen_expr(*expr.dot.object);
            }

            if (stype) {
                std::string struct_name = strip_struct_prefix(stype->getStructName().str());
                auto fit = struct_fields_.find(struct_name);
                if (fit != struct_fields_.end()) {
                    int field_idx = -1;
                    for (size_t i = 0; i < fit->second.size(); i++) {
                        if (fit->second[i] == expr.dot.field) {
                            field_idx = i;
                            break;
                        }
                    }
                    if (field_idx >= 0) {
                        llvm::Value* field_ptr = builder_->CreateGEP(stype, obj,
                            {llvm::ConstantInt::get(*context_, llvm::APInt(32, 0)),
                             llvm::ConstantInt::get(*context_, llvm::APInt(32, field_idx))},
                            expr.dot.field);
                        return builder_->CreateLoad(stype->getElementType(field_idx), field_ptr, expr.dot.field + ".val");
                    }
                    std::cerr << "error: unknown field '" << expr.dot.field << "'" << std::endl;
                    return llvm::ConstantInt::get(*context_, llvm::APInt(64, 0));
                }
            }
            std::cerr << "error: cannot access field '" << expr.dot.field << "' on non-struct type" << std::endl;
            return llvm::ConstantInt::get(*context_, llvm::APInt(64, 0));
        }

        case ExprKind::INDEX:
        case ExprKind::SLICE:
        case ExprKind::SIZEOF:
            return llvm::ConstantInt::get(*context_, llvm::APInt(64, 0));
        case ExprKind::LEN: {
            llvm::Value* val = gen_expr(*expr.len.arg);
            if (val->getType()->isStructTy()) {
                return builder_->CreateExtractValue(val, 1, "len");
            }
            return llvm::ConstantInt::get(*context_, llvm::APInt(config_.types.int_width, 0));
        }

        case ExprKind::POSTFIX_INC:
        case ExprKind::POSTFIX_DEC: {
            auto& pf = expr.postfix;
            auto& target = pf.operand;
            if (target->kind == ExprKind::IDENT) {
                auto it = named_values_.find(target->ident);
                if (it != named_values_.end()) {
                    llvm::Type* alloc_ty = it->second->getAllocatedType();
                    llvm::Value* cur = builder_->CreateLoad(alloc_ty, it->second, target->ident + ".old");
                    llvm::Value* one;
                    if (alloc_ty->isIntegerTy()) {
                        one = llvm::ConstantInt::get(*context_, llvm::APInt(64, 1));
                    } else {
                        one = llvm::ConstantFP::get(*context_, llvm::APFloat(1.0));
                    }
                    llvm::Value* new_val;
                    if (expr.kind == ExprKind::POSTFIX_INC) {
                        new_val = builder_->CreateAdd(cur, one, target->ident + ".inc");
                    } else {
                        new_val = builder_->CreateSub(cur, one, target->ident + ".dec");
                    }
                    builder_->CreateStore(new_val, it->second);
                    return cur;
                }
            } else if (target->kind == ExprKind::DOT_ACCESS) {
                std::string obj_name = "";
                if (target->dot.object->kind == ExprKind::IDENT) {
                    obj_name = target->dot.object->ident;
                }
                llvm::StructType* stype = nullptr;
                llvm::Value* obj = nullptr;
                if (!obj_name.empty()) stype = find_struct_type(obj_name);
                if (!obj_name.empty()) {
                    auto ait = named_values_.find(obj_name);
                    if (ait != named_values_.end()) {
                        llvm::Type* alloc_type = ait->second->getAllocatedType();
                        if (alloc_type->isStructTy()) {
                            stype = llvm::cast<llvm::StructType>(alloc_type);
                            obj = ait->second;
                        } else if (alloc_type->isPointerTy()) {
                            if (stype) obj = builder_->CreateLoad(alloc_type, ait->second, obj_name);
                        }
                    }
                }
                if (!obj) obj = gen_expr(*target->dot.object);
                if (stype) {
                    std::string struct_name = strip_struct_prefix(stype->getStructName().str());
                    auto fit = struct_fields_.find(struct_name);
                    if (fit != struct_fields_.end()) {
                        int field_idx = -1;
                        for (size_t i = 0; i < fit->second.size(); i++) {
                            if (fit->second[i] == target->dot.field) { field_idx = i; break; }
                        }
                        if (field_idx >= 0) {
                            llvm::Value* field_ptr = builder_->CreateGEP(stype, obj,
                                {llvm::ConstantInt::get(*context_, llvm::APInt(32, 0)),
                                 llvm::ConstantInt::get(*context_, llvm::APInt(32, field_idx))},
                                target->dot.field);
                            llvm::Value* cur = builder_->CreateLoad(stype->getElementType(field_idx), field_ptr, target->dot.field + ".old");
                            llvm::Value* one = llvm::ConstantInt::get(*context_, llvm::APInt(64, 1));
                            llvm::Value* new_val;
                            if (expr.kind == ExprKind::POSTFIX_INC) {
                                new_val = builder_->CreateAdd(cur, one, target->dot.field + ".inc");
                            } else {
                                new_val = builder_->CreateSub(cur, one, target->dot.field + ".dec");
                            }
                            builder_->CreateStore(new_val, field_ptr);
                            return cur;
                        }
                    }
                }
            }
            std::cerr << "error: invalid operand for postfix " << (expr.kind == ExprKind::POSTFIX_INC ? "++" : "--") << std::endl;
            return llvm::ConstantInt::get(*context_, llvm::APInt(64, 0));
        }

        case ExprKind::STRUCT_LITERAL: {
            std::string lookup_name = expr.struct_literal.type_name;

            // Generic struct literal: monomorphize
            if (!expr.struct_literal.type_args.empty()) {
                auto gen_it = generic_type_decls_.find(lookup_name);
                if (gen_it != generic_type_decls_.end()) {
                    std::vector<llvm::Type*> type_args;
                    for (auto& ta : expr.struct_literal.type_args) {
                        type_args.push_back(resolve_type(*ta));
                    }
                    llvm::StructType* specialized = monomorphize_type(lookup_name, type_args);
                    if (!specialized) {
                        return llvm::ConstantInt::get(*context_, llvm::APInt(64, 0));
                    }

                    llvm::AllocaInst* alloca = builder_->CreateAlloca(specialized, nullptr, lookup_name + ".tmp");
                    auto fit = struct_fields_.find(
                        mangle_generic(lookup_name, type_args));
                    if (fit == struct_fields_.end()) {
                        return alloca;
                    }
                    auto& field_names = fit->second;

                    for (auto& field_init : expr.struct_literal.fields) {
                        int field_idx = -1;
                        for (size_t i = 0; i < field_names.size(); i++) {
                            if (field_names[i] == field_init.name) {
                                field_idx = i;
                                break;
                            }
                        }
                        if (field_idx == -1) continue;
                        llvm::Value* val = gen_expr(*field_init.value);
                        llvm::Value* field_ptr = builder_->CreateGEP(specialized, alloca,
                            {llvm::ConstantInt::get(*context_, llvm::APInt(32, 0)),
                             llvm::ConstantInt::get(*context_, llvm::APInt(32, field_idx))},
                            field_init.name);
                        builder_->CreateStore(val, field_ptr);
                    }

                    alloca->setName(lookup_name + ".val");
                    value_struct_type_[alloca] = specialized;
                    return alloca;
                }
            }

            auto it = struct_types_.find(lookup_name);
            if (it == struct_types_.end()) {
                std::cerr << "error: unknown type '" << expr.struct_literal.type_name << "'" << std::endl;
                return llvm::ConstantInt::get(*context_, llvm::APInt(64, 0));
            }
            llvm::StructType* stype = it->second;
            auto fit = struct_fields_.find(expr.struct_literal.type_name);
            auto& field_names = fit->second;

            llvm::AllocaInst* alloca = builder_->CreateAlloca(stype, nullptr, expr.struct_literal.type_name + ".tmp");

            for (auto& field_init : expr.struct_literal.fields) {
                int field_idx = -1;
                for (size_t i = 0; i < field_names.size(); i++) {
                    if (field_names[i] == field_init.name) {
                        field_idx = i;
                        break;
                    }
                }
                if (field_idx == -1) {
                    std::cerr << "error: unknown field '" << field_init.name << "' in type '" << expr.struct_literal.type_name << "'" << std::endl;
                    continue;
                }
                llvm::Value* val = gen_expr(*field_init.value);
                llvm::Value* field_ptr = builder_->CreateGEP(stype, alloca,
                    {llvm::ConstantInt::get(*context_, llvm::APInt(32, 0)),
                     llvm::ConstantInt::get(*context_, llvm::APInt(32, field_idx))},
                    field_init.name);
                builder_->CreateStore(val, field_ptr);
            }

            alloca->setName(expr.struct_literal.type_name + ".val");
            value_struct_type_[alloca] = stype;
            return alloca;
        }
    }
    return nullptr;
}

llvm::Type* CodegenImpl::resolve_type(TypeAnnotation& type) {
    switch (type.kind) {
        case TypeKind::NAMED: {
            auto it = struct_types_.find(type.name);
            if (it != struct_types_.end()) {
                return it->second;
            }
            // Generic type with type args: Pair[int] → monomorphize
            if (!type.type_args.empty()) {
                std::vector<llvm::Type*> resolved_args;
                for (auto& ta : type.type_args) {
                    resolved_args.push_back(resolve_type(*ta));
                }
                llvm::StructType* specialized = monomorphize_type(type.name, resolved_args);
                if (specialized) return specialized;
            }
            if (type.name == "int" || type.name == "i32" || type.name == "i64" ||
                type.name == "u8" || type.name == "u16" || type.name == "u32" || type.name == "u64")
                return llvm::Type::getIntNTy(*context_, config_.types.int_width);
            if (type.name == "float" || type.name == "f32" || type.name == "f64")
                return llvm::Type::getDoubleTy(*context_);
            if (type.name == "bool")
                return llvm::Type::getIntNTy(*context_, config_.types.bool_width);
            if (type.name == "string")
                return string_type_;
            if (type.name == "char")
                return llvm::Type::getIntNTy(*context_, config_.types.char_width);
            if (type.name == "error") {
                if (error_tu_type_) return error_tu_type_;
                return llvm::PointerType::get(*context_, 0);
            }
            auto structural_it = structural_tu_types_.find(type.name);
            if (structural_it != structural_tu_types_.end()) {
                return structural_it->second;
            }
            return llvm::Type::getIntNTy(*context_, config_.types.int_width);
        }
        case TypeKind::POINTER:
        case TypeKind::SLICE:
        case TypeKind::ARRAY:
            return llvm::PointerType::get(*context_, 0);
        case TypeKind::TYPE_PARAM: {
            auto it = current_type_subst_.find(type.name);
            if (it != current_type_subst_.end()) return it->second;
            error("unresolved type parameter '" + type.name + "'");
            return llvm::Type::getInt64Ty(*context_);
        }
        case TypeKind::STRUCTURAL: {
            auto it = structural_tu_types_.find(type.name);
            if (it != structural_tu_types_.end()) return it->second;
            return llvm::PointerType::get(*context_, 0);
        }
        default:
            return llvm::Type::getInt64Ty(*context_);
    }
}

llvm::Type* CodegenImpl::resolve_type_by_name(const std::string& name) {
    TypeAnnotation ta;
    ta.kind = TypeKind::NAMED;
    ta.name = name;
    return resolve_type(ta);
}

} // namespace binar
