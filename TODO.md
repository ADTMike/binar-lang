# binar TODO

## Standard Library
- [ ] Print/println functions (basic I/O)
- [ ] String utilities (length, concat, compare)
- [ ] Type conversion (int to string, etc.)
- [ ] Error wrapping utilities
- [ ] Common data structures (dynamic array, hash map)
- [ ] OS interaction (file I/O, exit codes beyond return)
- [ ] Memory allocation (heap allocator)

## Build Tooling
- [ ] `binar build` command (compile directory, manage modules)
- [ ] `binar run` command (compile + run)
- [ ] `binar init` command (scaffold new project with binar.mod)
- [ ] `binar test` command (run tests in directory)
- [ ] Package registry / dependency resolution

## Language Features
- [ ] `defer` — block-scoped finalization (currently function-scoped)
- [ ] `enum` — C-style enums
- [ ] `union` — tagged unions / sum types
- [ ] Generics / type parameters (compile-time polymorphism)
- [ ] String type — more complete than current char-array approach
- [ ] Slice type — fat pointer with length + capacity
- [ ] Range-based for loop
- [ ] Default parameters
- [ ] Variadic functions
- [ ] Operator overloading
- [ ] Method declarations (not just function calls)
- [ ] Type inference improvements
- [ ] Comprehensive compile-time evaluation

## Compiler Improvements
- [ ] Error messages with source location and context
- [ ] Better type checking in sema (currently minimal)
- [ ] Semantic validation of `raise` (must be multi-return with error type)
- [ ] Dead code elimination
- [ ] Optimization passes (-O1, -O2, -Os)
- [ ] Incremental compilation
- [ ] LSP support (go-to-definition, autocomplete)
- [ ] Cross-compilation support
- [ ] WASM backend
- [ ] Windows/macOS support (currently Linux x86_64 only)

## Testing
- [ ] More edge-case tests for `raise` and `defer`
- [ ] Integration tests for module system
- [ ] Benchmark suite
- [ ] Fuzz testing
