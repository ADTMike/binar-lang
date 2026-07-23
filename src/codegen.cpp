#include "codegen.h"
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
    struct ImportEntry {
        std::string package_path;  // empty = same package
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

    // Interface and monomorphization support
    std::set<std::string> interface_names_;
    std::map<std::string, FnDecl> iface_fn_decls_;  // Functions with interface params (stored for monomorphization)
    std::map<std::string, FnDecl*> iface_returning_decls_;  // Functions returning interface (stored for inlining)
    struct InterfaceImpl {
        std::string interface_name;
        std::string concrete_type;
        std::map<std::string, llvm::Function*> method_impls;
    };
    std::vector<InterfaceImpl> interface_impls_;
    std::map<std::string, llvm::Function*> monomorphized_fns_;
    std::map<std::string, llvm::Type*> type_params_;

    struct InterfaceParam {
        std::string param_name;
        std::string interface_name;
    };
    std::vector<InterfaceParam> current_interface_params_;

    struct LoopContext {
        llvm::BasicBlock* break_target;
        llvm::BasicBlock* continue_target;
    };
    std::vector<LoopContext> loop_stack_;

    std::vector<Expr*> deferred_calls_;

    // Inlining state
    bool inlining_ = false;        // true while generating inlined function body
    bool inlining_raise_ = false;  // true if inlining in a raise context
    llvm::BasicBlock* inlining_exit_bb_ = nullptr;  // block to branch to on return during inlining
    llvm::AllocaInst* inlining_ret_alloca_ = nullptr;  // alloca for storing return value during non-raise inlining
    bool current_stmt_raise_ = false;  // set by gen_stmt when processing a raise expression

    std::vector<std::string> errors_;

    void error(const std::string& msg) {
        errors_.push_back(msg);
    }

    bool has_errors() const { return !errors_.empty(); }

    CodegenImpl() : current_fn_(nullptr) {
        context_ = std::make_unique<llvm::LLVMContext>();
        module_ = std::make_unique<llvm::Module>("binar", *context_);
        builder_ = std::make_unique<llvm::IRBuilder<>>(*context_);

        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86Target();
        LLVMInitializeX86TargetMC();
        LLVMInitializeX86AsmParser();
        LLVMInitializeX86AsmPrinter();
    }

    void gen_decl(Decl& decl);
    void gen_fn_decl(FnDecl& fn);
    void gen_type_decl(TypeDecl& td);
    void gen_iface_decl(InterfaceDecl& id);
    void gen_stmt(Stmt& stmt);
    bool generate_lazy(const std::string& package, const std::string& file);
    llvm::Value* gen_expr(Expr& expr);
    llvm::Type* resolve_type(TypeAnnotation& type);
    llvm::Type* resolve_type_by_name(const std::string& name);
    void gen_entry_point();
    llvm::StructType* find_struct_type(const std::string& name);
    std::string extract_type_name(TypeAnnotation* type);
    llvm::Function* gen_monomorphized_fn(const std::string& base_name,
                                          const std::string& concrete_type,
                                          FnDecl& original_fn);
    std::string infer_concrete_type(Expr* arg_expr);
    void emit_raise_check(llvm::Value* result);
    void emit_deferred();
    llvm::Value* gen_inline_call(FnDecl& fn, std::vector<llvm::Value*>& args, bool is_raise);
};

Codegen::Codegen() : impl_(std::make_unique<CodegenImpl>()) {}
Codegen::~Codegen() = default;

bool Codegen::generate(Program& program, const std::string& filename) {
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
    auto target_triple = llvm::sys::getDefaultTargetTriple();
    impl_->module_->setTargetTriple(llvm::Triple(target_triple));

    std::string error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(target_triple, error);
    if (!target) {
        llvm::errs() << error;
        return false;
    }

    llvm::TargetOptions options;
    auto* target_machine = target->createTargetMachine(
        llvm::Triple(target_triple), "generic", "", options, llvm::Reloc::PIC_);

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

// ==================== CodegenImpl ====================

void CodegenImpl::gen_decl(Decl& decl) {
    switch (decl.kind) {
        case DeclKind::FN: gen_fn_decl(decl.fn); break;
        case DeclKind::TYPE: gen_type_decl(decl.type_decl); break;
        case DeclKind::IFACE: gen_iface_decl(decl.iface_decl); break;
        case DeclKind::CONST: {
            auto& cd = decl.const_decl;
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

    // Check if any param has an interface type — if so, store for monomorphization
    bool has_iface_param = false;
    for (auto& param : fn.params) {
        std::string pname = extract_type_name(param.type.get());
        if (!pname.empty() && interface_names_.count(pname) > 0) {
            has_iface_param = true;
            break;
        }
    }
    if (has_iface_param) {
        iface_fn_decls_.emplace(fn.name, std::move(fn));
        return;
    }

    // Interface-returning functions are never emitted as standalone LLVM functions.
    // Their body is inlined at every call site instead.
    if (fn.is_interface_returning) {
        iface_returning_decls_[fn.name] = &fn;
        return;
    }

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
            // Return zeroed struct
            builder_->CreateRet(llvm::Constant::getNullValue(return_type));
        } else {
            builder_->CreateRet(llvm::ConstantInt::get(*context_, llvm::APInt(64, 0)));
        }
    }

    current_fn_ = nullptr;

    // Store function info for monomorphization
    // We'll check during call sites if this function has interface params
    llvm::verifyFunction(*func, &llvm::errs());
}

void CodegenImpl::gen_type_decl(TypeDecl& td) {
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

void CodegenImpl::gen_iface_decl(InterfaceDecl& id) {
    // Store interface name for monomorphization
    interface_names_.insert(id.name);
}

bool CodegenImpl::generate_lazy(const std::string& package, const std::string& file) {
    auto pit = pending_files_.find(package);
    if (pit == pending_files_.end()) return false;
    auto fit = pit->second.find(file);
    if (fit == pit->second.end()) return false;
    if (fit->second.codegen_done) return true;

    fit->second.codegen_done = true;

    // Save and restore context so caller's context is preserved
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

llvm::Function* CodegenImpl::gen_monomorphized_fn(
    const std::string& base_name,
    const std::string& concrete_type,
    FnDecl& original_fn) {

    std::string mono_name = base_name + "__" + concrete_type;

    // Check cache
    if (monomorphized_fns_.count(mono_name)) {
        return monomorphized_fns_[mono_name];
    }

    // Create new function type with concrete type replacing interface params
    std::vector<llvm::Type*> param_types;
    for (auto& param : original_fn.params) {
        std::string param_type_name = extract_type_name(param.type.get());
        bool is_iface = interface_names_.count(param_type_name) > 0;
        if (is_iface) {
            param_types.push_back(resolve_type_by_name(concrete_type));
        } else {
            param_types.push_back(resolve_type(*param.type));
        }
    }

    // Determine return type
    llvm::Type* return_type;
    bool is_multi_return = original_fn.return_types.size() > 1;
    if (is_multi_return) {
        std::vector<llvm::Type*> ret_field_types;
        for (auto& rt : original_fn.return_types) {
            ret_field_types.push_back(resolve_type(*rt));
        }
        return_type = llvm::StructType::get(*context_, ret_field_types, true);
    } else if (original_fn.return_types.empty()) {
        return_type = llvm::Type::getVoidTy(*context_);
    } else {
        return_type = resolve_type(*original_fn.return_types[0]);
    }

    // Create the monomorphized function
    llvm::FunctionType* fn_type = llvm::FunctionType::get(return_type, param_types, false);
    llvm::Function* mono_func = llvm::Function::Create(
        fn_type, llvm::Function::ExternalLinkage, mono_name, module_.get());

    // Set parameter names
    unsigned idx = 0;
    for (auto& param : original_fn.params) {
        mono_func->getArg(idx++)->setName(param.name);
    }

    // Save state BEFORE creating new function
    llvm::Function* prev_fn = current_fn_;
    llvm::BasicBlock* prev_block = builder_->GetInsertBlock();
    auto prev_named_values = named_values_;
    auto prev_named_struct_types = named_struct_types_;
    auto prev_deferred = std::move(deferred_calls_);

    // Create entry block
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context_, "entry", mono_func);
    builder_->SetInsertPoint(entry);
    current_fn_ = mono_func;
    deferred_calls_.clear();
    named_values_.clear();
    named_struct_types_.clear();

    // Allocate and store parameters
    idx = 0;
    for (auto& param : original_fn.params) {
        std::string param_type_name = extract_type_name(param.type.get());
        bool is_iface = interface_names_.count(param_type_name) > 0;

        llvm::Type* ptype;
        if (is_iface) {
            ptype = resolve_type_by_name(concrete_type);
        } else {
            ptype = resolve_type(*param.type);
        }

        llvm::AllocaInst* alloca = builder_->CreateAlloca(ptype, nullptr, param.name);
        builder_->CreateStore(mono_func->getArg(idx), alloca);
        named_values_[param.name] = alloca;

        // Track struct types for dot access
        if (is_iface) {
            auto sit = struct_types_.find(concrete_type);
            if (sit != struct_types_.end()) {
                named_struct_types_[param.name] = sit->second;
            }
        } else if (param.type->kind == TypeKind::NAMED) {
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

        idx++;
    }

    // Generate body — the original function's body with interface params resolved
    // We need to re-generate the body with the concrete type
    for (auto& stmt : original_fn.body) {
        gen_stmt(*stmt);
    }

    // Add default return if no terminator
    if (!builder_->GetInsertBlock()->getTerminator()) {
        emit_deferred();
        if (is_multi_return) {
            // Zero-initialized struct
            llvm::Type* ret_ty = mono_func->getReturnType();
            llvm::Value* zero = llvm::Constant::getNullValue(ret_ty);
            builder_->CreateRet(zero);
        } else if (original_fn.return_types.empty()) {
            builder_->CreateRetVoid();
        } else {
            llvm::Type* ret_ty = mono_func->getReturnType();
            llvm::Value* zero = llvm::Constant::getNullValue(ret_ty);
            builder_->CreateRet(zero);
        }
    }

    // Restore state
    current_fn_ = prev_fn;
    named_values_ = prev_named_values;
    named_struct_types_ = prev_named_struct_types;
    deferred_calls_ = std::move(prev_deferred);
    builder_->SetInsertPoint(prev_block);

    // Cache the monomorphized function
    monomorphized_fns_[mono_name] = mono_func;

    return mono_func;
}

std::string CodegenImpl::infer_concrete_type(Expr* arg_expr) {
    if (!arg_expr) return "";
    if (arg_expr->kind == ExprKind::STRUCT_LITERAL) {
        return arg_expr->struct_literal.type_name;
    }
    if (arg_expr->kind == ExprKind::IDENT) {
        auto it = named_struct_types_.find(arg_expr->ident);
        if (it != named_struct_types_.end()) {
            std::string sname = it->second->getStructName().str();
            if (sname.rfind("struct.", 0) == 0) sname = sname.substr(7);
            return sname;
        }
    }
    if (arg_expr->kind == ExprKind::UNARY && arg_expr->unary.op == UnOp::ADDR_OF) {
        if (arg_expr->unary.operand->kind == ExprKind::IDENT) {
            auto it = named_struct_types_.find(arg_expr->unary.operand->ident);
            if (it != named_struct_types_.end()) {
                std::string sname = it->second->getStructName().str();
                if (sname.rfind("struct.", 0) == 0) sname = sname.substr(7);
                return "*" + sname;
            }
        }
    }
    return "";
}

void CodegenImpl::gen_entry_point() {
    if (module_->getFunction("_start")) return;

    llvm::Function* main_fn = module_->getFunction("main");
    if (!main_fn) return;

    llvm::FunctionType* start_type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(*context_), false);
    llvm::Function* start_fn = llvm::Function::Create(
        start_type, llvm::Function::ExternalLinkage, "_start", module_.get());

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context_, "entry", start_fn);
    builder_->SetInsertPoint(entry);

    llvm::Value* ret_code;
    if (main_fn->getReturnType()->isVoidTy()) {
        builder_->CreateCall(main_fn);
        ret_code = llvm::ConstantInt::get(*context_, llvm::APInt(64, 0));
    } else {
        ret_code = builder_->CreateCall(main_fn);
    }

    // Linux x86_64 exit syscall via inline asm
    llvm::FunctionType* exit_type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(*context_),
        {llvm::Type::getInt64Ty(*context_)}, false);
    llvm::InlineAsm* exit_asm = llvm::InlineAsm::get(
        exit_type,
        "movq $$60, %rax\n\tmovq $0, %rdi\n\tsyscall",
        "r,~{rax},~{rdi},~{rcx},~{r11}",
        true);
    builder_->CreateCall(exit_asm, {ret_code});

    builder_->CreateUnreachable();
}

void CodegenImpl::emit_raise_check(llvm::Value* result) {
    llvm::StructType* sty = llvm::cast<llvm::StructType>(result->getType());
    unsigned n = sty->getNumElements();
    llvm::Value* err_val = builder_->CreateExtractValue(result, {n - 1}, "raise.err");

    llvm::Value* is_err = builder_->CreateICmpNE(err_val,
        llvm::ConstantInt::get(*context_, llvm::APInt(64, 0)), "raise.cond");

    llvm::Function* fn = builder_->GetInsertBlock()->getParent();
    llvm::BasicBlock* err_bb = llvm::BasicBlock::Create(*context_, "raise.ret", fn);
    llvm::BasicBlock* cont_bb = llvm::BasicBlock::Create(*context_, "raise.cont", fn);

    builder_->CreateCondBr(is_err, err_bb, cont_bb);

    builder_->SetInsertPoint(err_bb);
    emit_deferred();
    llvm::Type* ret_type = fn->getReturnType();
    if (ret_type->isVoidTy()) {
        builder_->CreateRetVoid();
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

llvm::Value* CodegenImpl::gen_inline_call(FnDecl& fn, std::vector<llvm::Value*>& args, bool is_raise) {
    // Save state
    auto saved_named_values = named_values_;
    auto saved_named_struct_types = named_struct_types_;
    auto saved_deferred = std::move(deferred_calls_);
    bool saved_inlining = inlining_;
    bool saved_inlining_raise = inlining_raise_;
    llvm::BasicBlock* saved_exit_bb = inlining_exit_bb_;
    llvm::AllocaInst* saved_ret_alloca = inlining_ret_alloca_;
    llvm::BasicBlock* resume_block = builder_->GetInsertBlock();

    // Create parameter allocas
    for (size_t i = 0; i < fn.params.size() && i < args.size(); i++) {
        std::string pname = "__inline_" + fn.name + "_" + fn.params[i].name;
        llvm::Type* ptype = resolve_type(*fn.params[i].type);
        llvm::AllocaInst* alloca = builder_->CreateAlloca(ptype, nullptr, pname);
        builder_->CreateStore(args[i], alloca);
        named_values_[fn.params[i].name] = alloca;
        named_struct_types_[fn.params[i].name] = named_struct_types_[fn.params[i].name];
    }

    // Set up inlining state
    inlining_ = true;
    inlining_raise_ = is_raise;

    llvm::Function* caller_fn = builder_->GetInsertBlock()->getParent();

    if (is_raise) {
        // For raise: RETURN in inlined body → return from caller function
        // We just let gen_stmt RETURN emit normally; it will return from the caller
        // because current_fn_ is still the caller
    } else {
        // For non-raise: RETURN in inlined body → branch to exit block
        llvm::BasicBlock* exit_bb = llvm::BasicBlock::Create(*context_, "inline.exit", caller_fn);
        llvm::Type* error_type = llvm::Type::getInt64Ty(*context_); // error is i64
        llvm::AllocaInst* ret_alloca = builder_->CreateAlloca(error_type, nullptr, "__inline_ret");
        inlining_exit_bb_ = exit_bb;
        inlining_ret_alloca_ = ret_alloca;
    }

    // Generate inlined body
    for (auto& stmt : fn.body) {
        gen_stmt(*stmt);
    }

    // If inlined body didn't terminate (reached end without return), it's a success path
    if (!builder_->GetInsertBlock()->getTerminator()) {
        if (is_raise) {
            // Dead block after inline raise return or fall-through without return
            emit_deferred();
            new llvm::UnreachableInst(*context_, builder_->GetInsertBlock());
        } else {
            // Non-raise: store 0 (nil/success) and branch to exit
            builder_->CreateStore(llvm::ConstantInt::get(*context_, llvm::APInt(64, 0)),
                                  inlining_ret_alloca_);
            emit_deferred();
            builder_->CreateBr(inlining_exit_bb_);
        }
    }

    // Restore state
    deferred_calls_ = std::move(saved_deferred);
    inlining_ = saved_inlining;
    inlining_raise_ = saved_inlining_raise;

    llvm::Value* result = nullptr;
    if (!is_raise && inlining_exit_bb_) {
        // Position builder at exit block and load result
        builder_->SetInsertPoint(inlining_exit_bb_);
        result = builder_->CreateLoad(llvm::Type::getInt64Ty(*context_), inlining_ret_alloca_, "__inline_ret_val");
    }

    // Restore inlining context (exit block / ret alloca) for outer inline
    inlining_exit_bb_ = saved_exit_bb;
    inlining_ret_alloca_ = saved_ret_alloca;

    named_values_ = saved_named_values;
    named_struct_types_ = saved_named_struct_types;

    return result;
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
                                    val = builder_->CreateExtractValue(val, {0});
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
                            } else if (assign.value && assign.value->kind == ExprKind::IDENT) {
                                auto sit = named_struct_types_.find(assign.value->ident);
                                if (sit != named_struct_types_.end()) {
                                    named_struct_types_[name] = sit->second;
                                }
                            }
                        } else {
                            // Inlined raise already returned from function; create unreachable
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
                                val = builder_->CreateExtractValue(val, {0});
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
                                std::string struct_name = stype->getStructName().str();
                                if (struct_name.rfind("struct.", 0) == 0) struct_name = struct_name.substr(7);
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
                        llvm::BasicBlock* unreachable = llvm::BasicBlock::Create(*context_, "unreachable.after.raise", current_fn_);
                        builder_->SetInsertPoint(unreachable);
                    }
                }
            }
            break;

        case StmtKind::RETURN:
            if (inlining_ && !inlining_raise_) {
                // Non-raise inlining: RETURN → branch to exit block with value
                if (!stmt.return_values.empty()) {
                    llvm::Value* val = gen_expr(*stmt.return_values[0]);
                    // Store the return value (for error, it's the concrete error struct value)
                    // For now, treat as i64
                    if (val->getType()->isPointerTy()) {
                        val = builder_->CreatePtrToInt(val, llvm::Type::getInt64Ty(*context_));
                    }
                    builder_->CreateStore(val, inlining_ret_alloca_);
                } else if (stmt.expr) {
                    llvm::Value* val = gen_expr(*stmt.expr);
                    if (val->getType()->isPointerTy()) {
                        val = builder_->CreatePtrToInt(val, llvm::Type::getInt64Ty(*context_));
                    }
                    builder_->CreateStore(val, inlining_ret_alloca_);
                } else {
                    // nil return
                    builder_->CreateStore(llvm::ConstantInt::get(*context_, llvm::APInt(64, 0)),
                                          inlining_ret_alloca_);
                }
                // Emit deferred calls before branching
                emit_deferred();
                builder_->CreateBr(inlining_exit_bb_);
                // Continue in unreachable block (statements after this return won't be reached)
                llvm::BasicBlock* unreachable = llvm::BasicBlock::Create(*context_, "inline.unreachable", builder_->GetInsertBlock()->getParent());
                builder_->SetInsertPoint(unreachable);
                break;
            }
            if (inlining_ && inlining_raise_) {
                // Raise inlining: RETURN → return from caller function
                // This is just a normal return since current_fn_ is the caller
                if (!stmt.return_values.empty()) {
                    llvm::Value* val = gen_expr(*stmt.return_values[0]);
                    if (val->getType()->isPointerTy()) {
                        val = builder_->CreatePtrToInt(val, llvm::Type::getInt64Ty(*context_));
                    }
                    emit_deferred();
                    builder_->CreateRet(val);
                } else if (stmt.expr) {
                    llvm::Value* val = gen_expr(*stmt.expr);
                    if (val->getType()->isPointerTy()) {
                        val = builder_->CreatePtrToInt(val, llvm::Type::getInt64Ty(*context_));
                    }
                    emit_deferred();
                    builder_->CreateRet(val);
                } else {
                    emit_deferred();
                    builder_->CreateRet(llvm::ConstantInt::get(*context_, llvm::APInt(64, 0)));
                }
                llvm::BasicBlock* unreachable2 = llvm::BasicBlock::Create(*context_, "inline.unreachable", builder_->GetInsertBlock()->getParent());
                builder_->SetInsertPoint(unreachable2);
                break;
            }
            if (!stmt.return_values.empty()) {
                // Multi-return: build packed struct with insertvalue
                llvm::Type* ret_type = current_fn_->getReturnType();
                llvm::Value* result = llvm::UndefValue::get(ret_type);
                for (size_t i = 0; i < stmt.return_values.size(); i++) {
                    llvm::Value* val = gen_expr(*stmt.return_values[i]);
                    // If return type expects a struct value but we have a pointer, load it
                    llvm::Type* elem_type = ret_type->isStructTy() ? ret_type :
                        (ret_type->isStructTy() ? ret_type : nullptr);
                    if (val->getType()->isPointerTy() && ret_type->isStructTy()) {
                        val = builder_->CreateLoad(ret_type, val, "retload");
                    }
                    result = builder_->CreateInsertValue(result, val, (unsigned)i);
                }
                emit_deferred();
                builder_->CreateRet(result);
            } else if (stmt.expr) {
                llvm::Value* val = gen_expr(*stmt.expr);
                llvm::Type* ret_type = current_fn_->getReturnType();
                // If return type is a struct value but we have a pointer, load it
                if (val->getType()->isPointerTy() && ret_type->isStructTy()) {
                    val = builder_->CreateLoad(ret_type, val, "retload");
                }
                emit_deferred();
                builder_->CreateRet(val);
            } else {
                emit_deferred();
                builder_->CreateRetVoid();
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
        case StmtKind::CONTINUE:
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
                    asm_type, stmt.asm_stmt.code, "", stmt.asm_stmt.is_volatile);
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
                    asm_type, stmt.asm_stmt.code, constraint_str, stmt.asm_stmt.is_volatile);
                builder_->CreateCall(inline_asm, arg_vals);
            }
            break;
        }

        case StmtKind::MULTI_ASSIGN: {
            llvm::Value* val = gen_expr(*stmt.multi_assign.value);

            // val should be a packed struct from multi-return function
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

        case ExprKind::STRING_LIT:
            return builder_->CreateGlobalString(expr.string_val);

        case ExprKind::BOOL_LIT:
            return llvm::ConstantInt::get(*context_, llvm::APInt(1, expr.bool_val ? 1 : 0));

        case ExprKind::NIL_LIT:
            return llvm::ConstantPointerNull::get(llvm::PointerType::get(*context_, 0));

        case ExprKind::IDENT: {
            auto it = named_values_.find(expr.ident);
            if (it != named_values_.end()) {
                return builder_->CreateLoad(it->second->getAllocatedType(), it->second, expr.ident);
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
                case BinOp::EQ: return builder_->CreateICmpEQ(left, right, "eqtmp");
                case BinOp::NEQ: return builder_->CreateICmpNE(left, right, "neqtmp");
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

            // Helper: check if a name is exported (starts with uppercase)
            auto is_exported = [](const std::string& s) -> bool {
                return !s.empty() && s[0] >= 'A' && s[0] <= 'Z';
            };

            // Helper: save/restore builder state for lazy compilation
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
                        // Import binding found: file.fn pattern
                        auto& entry = it->second;
                        std::string pkg = entry.package_path;
                        if (pkg.empty()) pkg = current_package_;
                        lazy_with_state(pkg, entry.file);
                        fn_name_mangled = entry.file + "." + fn_name_bare;
                        if (!entry.package_path.empty()) {
                            is_cross_package = true;
                        }
                    } else {
                        // Not an import — treat as method call or receiver expression
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

            // Special case: fmt.Print/Println with string literal
            if (fn_name_mangled == "fmt.Print" || fn_name_mangled == "fmt.Println") {
                if (!expr.call.args.empty() &&
                    expr.call.args[0]->kind == ExprKind::STRING_LIT) {
                    std::string str_val = expr.call.args[0]->string_val;
                    int str_len = (int)str_val.size();

                    llvm::FunctionType* write_type = llvm::FunctionType::get(
                        llvm::Type::getVoidTy(*context_),
                        {llvm::PointerType::get(*context_, 0)}, false);
                    llvm::InlineAsm* write_asm = llvm::InlineAsm::get(
                        write_type,
                        "movq $$1, %rax\n\tmovq $$1, %rdi\n\tmovq $0, %rsi\n\tmovq $$" + std::to_string(str_len) + ", %rdx\n\tsyscall",
                        "r,~{rax},~{rdi},~{rcx},~{r11}",
                        true);
                    builder_->CreateCall(write_asm, {args[0]});

                    if (fn_name_bare == "Println") {
                        llvm::Value* nl_str = builder_->CreateGlobalString("\n");
                        llvm::InlineAsm* nl_asm = llvm::InlineAsm::get(
                            write_type,
                            "movq $$1, %rax\n\tmovq $$1, %rdi\n\tmovq $0, %rsi\n\tmovq $$1, %rdx\n\tsyscall",
                            "r,~{rax},~{rdi},~{rcx},~{r11}",
                            true);
                        builder_->CreateCall(nl_asm, {nl_str});
                    }
                    return nullptr;
                }
            }

            // Visibility check: exported functions only for cross-package calls
            if (is_cross_package && !is_exported(fn_name_bare)) {
                error("cannot access unexported function '" + fn_name_bare + "'");
                return llvm::ConstantInt::get(*context_, llvm::APInt(64, 0));
            }

            // Interface-returning function: inline at call site
            if (!fn_name_bare.empty()) {
                auto iface_ret_it = iface_returning_decls_.find(fn_name_bare);
                if (iface_ret_it != iface_returning_decls_.end()) {
                    // Determine raise context: check if this CALL is inside a raise expression
                    bool call_is_raise = false;
                    // Walk up to find the parent statement's raise flag
                    // For standalone raise: Stmt::raise on an EXPR containing this CALL
                    // For assigned raise: AssignExpr::raise on a CALL value
                    // We detect this from the stmt being processed (passed via flag)
                    call_is_raise = current_stmt_raise_;
                    return gen_inline_call(*iface_ret_it->second, args, call_is_raise);
                }
            }

            llvm::Function* func = nullptr;

            // Monomorphization dispatch: check if function has interface params
            if (!fn_name_bare.empty()) {
                auto iface_it = iface_fn_decls_.find(fn_name_bare);
                if (iface_it != iface_fn_decls_.end()) {
                    Expr* first_arg = expr.call.args.empty() ? nullptr : expr.call.args[0].get();
                    std::string concrete = infer_concrete_type(first_arg);
                    if (!concrete.empty() && !concrete.starts_with("*")) {
                        func = gen_monomorphized_fn(fn_name_bare, concrete, iface_it->second);
                    } else if (!concrete.empty() && concrete.starts_with("*")) {
                        func = gen_monomorphized_fn(fn_name_bare, concrete.substr(1), iface_it->second);
                    }
                }
                if (!func) {
                    func = module_->getFunction(fn_name_mangled);
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

                if (!receiver_type->isPointerTy() && param_type->isPointerTy()) {
                    llvm::AllocaInst* alloca = builder_->CreateAlloca(receiver_type, nullptr, "methodtmp");
                    builder_->CreateStore(receiver, alloca);
                    receiver = alloca;
                } else if (receiver_type->isPointerTy() && !param_type->isPointerTy()) {
                    receiver = builder_->CreateLoad(param_type, receiver, "deref");
                }
                args.insert(args.begin(), receiver);
            }

            for (size_t i = 0; i < args.size() && i < func->arg_size(); i++) {
                llvm::Type* param_type = func->getArg(i)->getType();
                llvm::Type* arg_type = args[i]->getType();
                if (param_type->isStructTy() && arg_type->isPointerTy()) {
                    args[i] = builder_->CreateLoad(param_type, args[i], "structarg");
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
                std::string struct_name = stype->getStructName().str();
                if (struct_name.rfind("struct.", 0) == 0) struct_name = struct_name.substr(7);
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
                    std::string struct_name = stype->getStructName().str();
                    if (struct_name.rfind("struct.", 0) == 0) struct_name = struct_name.substr(7);
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
            auto it = struct_types_.find(expr.struct_literal.type_name);
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
            if (type.name == "int" || type.name == "i32" || type.name == "i64" ||
                type.name == "u8" || type.name == "u16" || type.name == "u32" || type.name == "u64")
                return llvm::Type::getInt64Ty(*context_);
            if (type.name == "float" || type.name == "f32" || type.name == "f64")
                return llvm::Type::getDoubleTy(*context_);
            if (type.name == "bool")
                return llvm::Type::getInt1Ty(*context_);
            if (type.name == "string")
                return llvm::PointerType::get(*context_, 0);
            if (type.name == "char")
                return llvm::Type::getInt8Ty(*context_);
            return llvm::Type::getInt64Ty(*context_);
        }
        case TypeKind::POINTER:
        case TypeKind::SLICE:
        case TypeKind::ARRAY:
            return llvm::PointerType::get(*context_, 0);
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
