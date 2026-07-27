#pragma once

#include <string>
#include <map>
#include <vector>
#include <cstdint>
#include <functional>

namespace binar {

// ==================== Struct Prefix Helpers ====================

inline constexpr const char* LLVM_STRUCT_PREFIX = "struct.";
inline constexpr size_t LLVM_STRUCT_PREFIX_LEN = 7;

inline std::string strip_struct_prefix(const std::string& name) {
    if (name.size() >= LLVM_STRUCT_PREFIX_LEN &&
        name.compare(0, LLVM_STRUCT_PREFIX_LEN, LLVM_STRUCT_PREFIX) == 0) {
        return name.substr(LLVM_STRUCT_PREFIX_LEN);
    }
    return name;
}

inline std::string add_struct_prefix(const std::string& name) {
    return std::string(LLVM_STRUCT_PREFIX) + name;
}

inline bool has_struct_prefix(const std::string& name) {
    return name.size() >= LLVM_STRUCT_PREFIX_LEN &&
           name.compare(0, LLVM_STRUCT_PREFIX_LEN, LLVM_STRUCT_PREFIX) == 0;
}

// ==================== Target Configuration ====================

enum class TargetArch {
    X86_64,
    AARCH64,
    WASM,
};

enum class TargetOS {
    LINUX,
    MACOS,
    WINDOWS,
    WASM,
};

struct SyscallDef {
    uint32_t number;
    std::vector<std::string> arg_regs;
    std::string clobbers;
};

struct SyscallABI {
    std::vector<std::string> arg_regs;
    std::string syscall_insn;
    std::string clobbers;
    std::map<std::string, SyscallDef> calls;
};

// ==================== Type Width Configuration ====================

struct TypeWidths {
    uint32_t int_width = 64;
    uint32_t float_width = 64;
    uint32_t bool_width = 1;
    uint32_t char_width = 8;
    uint32_t pointer_width = 64;
};

struct BuiltinPortMethod {
    std::string name;
    std::string param_type;
    bool is_pointer;
};

struct BuiltinPort {
    std::string name;
    std::vector<BuiltinPortMethod> methods;
};

// ==================== Entry Point Configuration ====================

struct EntryPointConfig {
    std::string symbol = "_start";
    bool uses_libc = false;
};

// ==================== Module Configuration ====================

struct ModuleConfig {
    std::string marker_file = "binar.mod";
    std::string source_ext = ".binar";
    std::string env_home_var = "BINAR_HOME";
    std::string std_subdir = "std";
};

// ==================== Target Init ====================

struct TargetInit {
    bool x86 = true;
    bool aarch64 = false;
    bool wasm = false;
};

// ==================== Constants ====================

struct CompilerConstants {
    std::string discard_name = "_";
    std::string mangle_separator = "__";
    std::string inline_prefix = "__inline_";
    std::string tail_tmp_prefix = "__tail_tmp_";
};

// ==================== Top-level Config ====================

struct CompilerConfig {
    TargetArch arch = TargetArch::X86_64;
    TargetOS os = TargetOS::LINUX;
    std::string target_triple;
    std::string cpu = "generic";

    TargetInit target_init;
    SyscallABI syscall;
    TypeWidths types;
    std::vector<BuiltinPort> builtin_ports;
    EntryPointConfig entry_point;
    ModuleConfig module;
    CompilerConstants constants;

    static CompilerConfig default_config();
    static CompilerConfig from_triple(const std::string& triple);
};

} // namespace binar
