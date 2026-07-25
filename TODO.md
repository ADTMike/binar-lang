# binar TODO

Internal development notes. See [README.md](README.md) for user-facing documentation.

## Current State

- 131/131 tests passing
- `main()` (void) and `main() error` supported; `main() int` removed
- `os.Exit(code)` built-in available
- Error interface inline + raise + defer working

## Standard Library

- [ ] String utilities (length, concat, compare)
- [ ] Type conversion (int to string, etc.)
- [ ] Error wrapping utilities
- [ ] Common data structures (dynamic array, hash map)
- [ ] OS interaction (file I/O)
- [ ] Memory allocation (heap allocator)

## Compiler

- [ ] Error messages with source location and context
- [ ] Better type checking in sema
- [ ] Dead code elimination
- [ ] Optimization passes (-O1, -O2, -Os)
- [ ] Incremental compilation
- [ ] Cross-compilation support
- [ ] WASM backend
- [ ] Windows/macOS support

## Language

- [ ] Block-scoped `defer`
- [ ] Enums
- [ ] Tagged unions / sum types
- [ ] Generics / type parameters
- [ ] Proper string type
- [ ] Slice type with length + capacity
- [ ] Range-based for loop
- [ ] Default parameters
- [ ] Variadic functions
- [ ] Operator overloading

## Testing

- [ ] More edge-case tests for `raise` and `defer`
- [ ] Integration tests for module system
- [ ] Benchmark suite
- [ ] Fuzz testing
