#include "lexer.h"
#include "parser.h"
#include "sema.h"
#include "codegen.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <filesystem>

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options] <input.binar>" << std::endl;
    std::cerr << "Options:" << std::endl;
    std::cerr << "  -o <file>    Output file (default: output.o)" << std::endl;
    std::cerr << "  -ir          Print LLVM IR instead of compiling" << std::endl;
    std::cerr << "  -tokens      Print tokens and exit" << std::endl;
    std::cerr << "  -ast         Print AST and exit" << std::endl;
    std::cerr << "  -h, --help   Show this help message" << std::endl;
}

std::string read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "error: cannot open file '" << path << "'" << std::endl;
        exit(1);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

struct Module {
    std::string root;
    std::string name;
    std::map<std::string, Module> deps;
};

struct ResolveResult {
    std::string dir;
    const Module* module;
};

static Module load_module_at(const std::string& mod_dir, std::set<std::string>& seen_roots) {
    std::string abs_dir = std::filesystem::absolute(mod_dir).string();
    if (std::filesystem::exists(abs_dir + "/binar.mod")) {
        if (seen_roots.count(abs_dir)) {
            std::cerr << "error: circular module dependency at '" << abs_dir << "'" << std::endl;
            exit(1);
        }
        seen_roots.insert(abs_dir);
    }

    std::string mod_path = abs_dir + "/binar.mod";
    if (!std::filesystem::exists(mod_path)) {
        std::cerr << "error: " << mod_path << " not found" << std::endl;
        exit(1);
    }

    std::ifstream mod_file(mod_path);
    std::string line;
    Module mod;
    mod.root = abs_dir;

    while (std::getline(mod_file, line)) {
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        if (line.substr(s, 2) == "//") continue;

        // module <name>
        if (line.substr(s, 7) == "module ") {
            std::string name = line.substr(s + 7);
            size_t end = name.find_last_not_of(" \t\r");
            if (end != std::string::npos) name = name.substr(0, end + 1);
            size_t comment = name.find("//");
            if (comment != std::string::npos) name = name.substr(0, comment);
            end = name.find_last_not_of(" \t\r");
            if (end != std::string::npos) name = name.substr(0, end + 1);
            if (!name.empty()) mod.name = name;
        }

        // require "identity" => "path"
        if (line.substr(s, 8) == "require ") {
            std::string rest = line.substr(s + 8);
            // Find first quoted token
            size_t q1 = rest.find('"');
            if (q1 == std::string::npos) continue;
            size_t q2 = rest.find('"', q1 + 1);
            if (q2 == std::string::npos) continue;
            std::string identity = rest.substr(q1 + 1, q2 - q1 - 1);

            // Find => or =
            size_t arrow = rest.find("=>", q2 + 1);
            if (arrow == std::string::npos) arrow = rest.find('=', q2 + 1);
            if (arrow == std::string::npos) continue;

            size_t q3 = rest.find('"', arrow + 1);
            if (q3 == std::string::npos) continue;
            size_t q4 = rest.find('"', q3 + 1);
            if (q4 == std::string::npos) continue;
            std::string path = rest.substr(q3 + 1, q4 - q3 - 1);

            std::string dep_dir = std::filesystem::absolute(std::filesystem::path(mod.root) / path).string();
            mod.deps[identity] = load_module_at(dep_dir, seen_roots);
        }
    }

    if (mod.name.empty()) {
        std::cerr << "error: " << mod_path << " is missing 'module <name>' declaration" << std::endl;
        exit(1);
    }

    return mod;
}

Module find_module_root(const std::string& start_dir) {
    std::string dir = std::filesystem::absolute(start_dir).string();
    while (true) {
        if (std::filesystem::exists(dir + "/binar.mod")) {
            std::set<std::string> seen;
            return load_module_at(dir, seen);
        }
        auto parent = std::filesystem::path(dir).parent_path();
        if (parent == dir) break;
        dir = parent.string();
    }
    return {};
}

ResolveResult resolve_package_dir(const std::string& import_path,
                                  const Module& mod) {
    // Check required modules first (longest prefix match)
    std::string best_identity;
    const Module* best_module = nullptr;
    for (auto& [identity, dep] : mod.deps) {
        if (import_path == identity || (import_path.size() > identity.size() &&
            import_path.substr(0, identity.size()) == identity &&
            import_path[identity.size()] == '/')) {
            if (identity.size() > best_identity.size()) {
                best_identity = identity;
                best_module = &dep;
            }
        }
    }
    if (best_module) {
        std::string remainder = import_path.substr(best_identity.size());
        if (!remainder.empty() && remainder[0] == '/') remainder = remainder.substr(1);
        std::string dir = best_module->root;
        if (!remainder.empty()) dir = dir + "/" + remainder;
        if (std::filesystem::is_directory(dir)) return {dir, best_module};
    }

    // Check current module name as prefix
    if (!mod.name.empty()) {
        std::string prefix = mod.name + "/";
        if (import_path == mod.name ||
            (import_path.size() > prefix.size() &&
             import_path.substr(0, prefix.size()) == prefix)) {
            std::string remainder;
            if (import_path == mod.name) {
                remainder = "";
            } else {
                remainder = import_path.substr(prefix.size());
            }
            std::string dir = mod.root;
            if (!remainder.empty()) dir = dir + "/" + remainder;
            if (std::filesystem::is_directory(dir)) return {dir, &mod};
        }
    }

    // Std library
    const char* binar_home = getenv("BINAR_HOME");
    if (binar_home) {
        std::string std_dir = std::string(binar_home) + "/std/" + import_path;
        if (std::filesystem::is_directory(std_dir)) return {std_dir, nullptr};
    }

    return {{}, nullptr};
}

struct ParsedFile {
    std::string logical_pkg;  // logical path from import (e.g. "102_module_import/math")
    std::string resolved_pkg; // resolved absolute directory path
    std::string file_basename;
    std::string full_path;
    std::string source;
    binar::Program program;
};

ParsedFile parse_binar_file(const std::string& file_path,
                            const std::string& logical_pkg,
                            const std::string& resolved_pkg,
                            const std::string& file_basename) {
    std::string src = read_file(file_path);
    binar::Lexer lex(src, file_path);
    auto toks = lex.tokenize();
    binar::Parser par(toks, file_path);
    binar::Program prog = par.parse();
    binar::Sema sema;
    if (!sema.analyze(prog)) {
        for (const auto& err : sema.errors()) {
            std::cerr << file_path << ":" << err.line << ":" << err.column
                      << ": error: " << err.message << std::endl;
        }
        exit(1);
    }
    return {logical_pkg, resolved_pkg, file_basename, file_path,
            std::move(src), std::move(prog)};
}

void process_imports(const std::string& parent_logical,
                     const std::string& parent_resolved,
                     binar::Program& prog,
                     std::vector<ParsedFile>& all_files,
                     std::set<std::string>& seen_paths,
                     const Module& mod,
                     bool is_root) {
    for (auto& decl : prog.decls) {
        if (decl.kind != binar::DeclKind::IMPORT) continue;
        for (auto& binding : decl.import_block.bindings) {
            std::string logical_pkg;
            std::string resolved_pkg;
            if (binding.package_path.empty()) {
                if (is_root) {
                    std::cerr << "error: same-package import (missing 'from') in root file"
                              << std::endl;
                    exit(1);
                }
                logical_pkg = parent_logical;
                resolved_pkg = parent_resolved;
            } else {
                logical_pkg = binding.package_path;
                auto res = resolve_package_dir(logical_pkg, mod);
                if (!res.module) {
                    std::cerr << "error: cannot find package '" << logical_pkg << "'" << std::endl;
                    exit(1);
                }
                resolved_pkg = res.dir;
            }

            std::string file_path = resolved_pkg + "/" + binding.name + ".binar";
            if (seen_paths.count(file_path)) continue;
            seen_paths.insert(file_path);

            // Determine which module this file belongs to
            const Module* file_mod = &mod;
            if (!binding.package_path.empty()) {
                auto res = resolve_package_dir(binding.package_path, mod);
                if (res.module) file_mod = res.module;
            }

            all_files.push_back(
                parse_binar_file(file_path, logical_pkg, resolved_pkg, binding.name));

            // Recurse: process imports using the file's module context
            process_imports(logical_pkg, resolved_pkg, all_files.back().program,
                           all_files, seen_paths, *file_mod, false);
        }
    }
}

int main(int argc, char* argv[]) {
    std::string input;
    std::string output = "output.o";
    bool emit_ir = false;
    bool dump_tokens = false;
    bool dump_ast = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            output = argv[++i];
        } else if (arg == "-ir") {
            emit_ir = true;
        } else if (arg == "-tokens") {
            dump_tokens = true;
        } else if (arg == "-ast") {
            dump_ast = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg[0] != '-') {
            input = arg;
        } else {
            std::cerr << "error: unknown option '" << arg << "'" << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    if (input.empty()) {
        std::cerr << "error: no input file" << std::endl;
        print_usage(argv[0]);
        return 1;
    }

    Module mod = find_module_root(std::filesystem::path(input).parent_path().string());

    // Parse main file
    std::string main_source = read_file(input);
    binar::Lexer main_lexer(main_source, input);
    auto main_tokens = main_lexer.tokenize();

    if (dump_tokens) {
        for (const auto& tok : main_tokens) {
            std::cout << "[" << token_type_name(tok.type) << "] "
                      << "'" << tok.value << "' "
                      << "at " << tok.line << ":" << tok.column << std::endl;
        }
        return 0;
    }

    binar::Parser main_parser(main_tokens, input);
    binar::Program main_program = main_parser.parse();

    // Sema main file
    {
        binar::Sema sema;
        if (!sema.analyze(main_program)) {
            for (const auto& err : sema.errors()) {
                std::cerr << input << ":" << err.line << ":" << err.column
                          << ": error: " << err.message << std::endl;
            }
            return 1;
        }
    }

    // Error: imports require a module
    if (mod.name.empty()) {
        for (auto& decl : main_program.decls) {
            if (decl.kind == binar::DeclKind::IMPORT) {
                std::cerr << "error: imports require a binar.mod with 'module <name>' declaration" << std::endl;
                return 1;
            }
        }
    }

    // Recursively process imports from all files
    std::vector<ParsedFile> all_files;
    std::set<std::string> seen_paths;
    seen_paths.insert(input);

    process_imports("", "", main_program, all_files, seen_paths, mod, true);

    // Code generation
    binar::Codegen codegen;

    // Register pending package files with logical paths (matching import syntax)
    for (auto& pf : all_files) {
        codegen.register_pending_file(pf.logical_pkg, pf.file_basename,
                                       pf.full_path, pf.source, std::move(pf.program));
    }

    // Generate code for main file
    if (!codegen.generate(main_program, input)) {
        std::cerr << "error: code generation failed" << std::endl;
        return 1;
    }

    if (emit_ir) {
        codegen.emit_ir(output);
    } else {
        if (!codegen.emit_object(output)) {
            std::cerr << "error: failed to emit object file" << std::endl;
            return 1;
        }
        std::cout << "compiled to " << output << std::endl;
    }

    return 0;
}
