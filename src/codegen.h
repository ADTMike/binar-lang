#pragma once

#include "ast.h"
#include <string>
#include <memory>

namespace binar {

class CodegenImpl;

class Codegen {
public:
    Codegen();
    ~Codegen();

    bool generate(Program& program, const std::string& filename);
    bool generate_imported(Program& program, const std::string& filename);
    bool emit_object(const std::string& output);
    bool emit_ir(const std::string& output);

    void register_pending_file(const std::string& package_name,
                                const std::string& file_basename,
                                const std::string& path,
                                const std::string& source,
                                Program program);

private:
    std::unique_ptr<CodegenImpl> impl_;
};

} // namespace binar
