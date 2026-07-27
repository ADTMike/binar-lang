cmake_minimum_required(VERSION 3.20)
project(binar VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find LLVM
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# Find required dependencies
find_package(ZLIB REQUIRED)
find_package(FFI)
find_package(LibXml2)

# Get required LLVM definitions and includes
add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})

# Collect LLVM needed libs
llvm_map_components_to_libnames(llvm_libs core support irreader analysis transformutils passes codegen x86codegen x86asmparser x86info armcodegen arminfo)

# Source files
set(SOURCES
    src/main.cpp
    src/lexer.cpp
    src/parser.cpp
    src/sema.cpp
    src/codegen.cpp
    src/config.cpp
)

add_executable(binar ${SOURCES})
target_link_libraries(binar PRIVATE ${llvm_libs} ZLIB::ZLIB)
target_include_directories(binar PRIVATE src/)
