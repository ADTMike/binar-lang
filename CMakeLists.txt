cmake_minimum_required(VERSION 3.20)
project(binar VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find LLVM
find_package(LLVM REQUIRED CONFIG HINTS /nix/store/rfx89pjxx0dzlw8rbl9h5i1xsfv137kv-llvm-21.1.8-dev/lib/cmake/llvm)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# Find required dependencies for LLVM
find_package(ZLIB QUIET HINTS /nix/store/fjas1zgi2xawi1aldkyn2d920jc1avcf-zlib-1.3.2-dev /nix/store/b2swxfi8srrbsafvh9iyyhd26mz9giwf-zlib-1.3.2)
if(NOT ZLIB_FOUND)
    # Manual zlib detection for NixOS
    find_path(ZLIB_INCLUDE_DIR zlib.h HINTS /nix/store/fjas1zgi2xawi1aldkyn2d920jc1avcf-zlib-1.3.2-dev/include)
    find_library(ZLIB_LIBRARY z HINTS /nix/store/b2swxfi8srrbsafvh9iyyhd26mz9giwf-zlib-1.3.2/lib)
    if(ZLIB_INCLUDE_DIR AND ZLIB_LIBRARY AND NOT TARGET ZLIB::ZLIB)
        set(ZLIB_FOUND TRUE)
        add_library(ZLIB::ZLIB UNKNOWN IMPORTED)
        set_target_properties(ZLIB::ZLIB PROPERTIES
            IMPORTED_LOCATION "${ZLIB_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ZLIB_INCLUDE_DIR}")
        message(STATUS "Found ZLIB: ${ZLIB_LIBRARY}")
    endif()
endif()
find_package(FFI)
find_package(LibXml2)

# Get required LLVM definitions and includes
add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})

# Collect LLVM needed libs
llvm_map_components_to_libnames(llvm_libs core support irreader analysis transformutils passes codegen x86codegen x86asmparser x86info)

# Source files
set(SOURCES
    src/main.cpp
    src/lexer.cpp
    src/parser.cpp
    src/sema.cpp
    src/codegen.cpp
)

add_executable(binar ${SOURCES})
target_link_libraries(binar PRIVATE ${llvm_libs} ZLIB::ZLIB)
target_include_directories(binar PRIVATE src/)
