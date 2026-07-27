#include "config.h"

namespace binar {

CompilerConfig CompilerConfig::default_config() {
    CompilerConfig cfg;

    cfg.arch = TargetArch::X86_64;
    cfg.os = TargetOS::LINUX;
    cfg.cpu = "generic";

    cfg.target_init = TargetInit{.x86 = true};

    cfg.syscall = SyscallABI{
        .arg_regs = {"rdi", "rsi", "rdx", "r10", "r8", "r9"},
        .syscall_insn = "syscall",
        .clobbers = "~{rax},~{rcx},~{r11}",
        .calls = {
            {"exit",  {60,  {"rdi"},   "~{rax},~{rdi},~{rcx},~{r11}"}},
            {"write", {1,   {"rsi"},   "~{rax},~{rdi},~{rcx},~{r11}"}},
        },
    };

    cfg.types = TypeWidths{
        .int_width = 64,
        .float_width = 64,
        .bool_width = 1,
        .char_width = 8,
        .pointer_width = 64,
    };

    cfg.builtin_ports = {
        BuiltinPort{
            "error",
            {{"Error", "error", false}},
        },
    };

    cfg.entry_point = EntryPointConfig{.symbol = "_start", .uses_libc = false};
    cfg.module = ModuleConfig{};

    cfg.constants = CompilerConstants{};

    return cfg;
}

CompilerConfig CompilerConfig::from_triple(const std::string& triple) {
    CompilerConfig cfg = default_config();
    cfg.target_triple = triple;

    if (triple.find("aarch64") != std::string::npos ||
        triple.find("arm64") != std::string::npos) {
        cfg.arch = TargetArch::AARCH64;
        cfg.target_init = TargetInit{.aarch64 = true};
    } else if (triple.find("wasm") != std::string::npos) {
        cfg.arch = TargetArch::WASM;
        cfg.target_init = TargetInit{.wasm = true};
        cfg.os = TargetOS::WASM;
    }

    if (triple.find("darwin") != std::string::npos ||
        triple.find("macos") != std::string::npos) {
        cfg.os = TargetOS::MACOS;
    } else if (triple.find("windows") != std::string::npos) {
        cfg.os = TargetOS::WINDOWS;
    }

    return cfg;
}

} // namespace binar
