# binar

A systems programming language with Go-Rust-Odin-Zig-inspired syntax and **zero-cost error abstraction**, compiled to native code via LLVM.

**Status**: v0.1.0 — early development. 131 tests, 126 passing.

## Overview

binar is a statically typed, compiled language targeting native x86_64 Linux executables. The compiler is written in C++20 and uses LLVM 21 for code generation.

The core design principle is **zero-cost abstractions for error handling**: functions that return `error` (or any interface) are inlined at every call site at compile time. No vtable lookups, no fat pointers, no hidden allocations — the compiler generates the same code you would write by hand.

### Key Features

- **Static typing** with structs, interfaces, pointers, arrays, slices, and fixed-width integers
- **Interface system** with Go-like structural typing (no `implements` keyword)
- **Zero-cost error abstraction** — error-returning functions are inlined, not called
- **`raise` keyword** for ergonomic error propagation
- **`defer`** for cleanup with LIFO ordering
- **Module system** with per-package visibility (uppercase = exported)
- **Monomorphization** for interface parameters
- **Method syntax** — dot notation as sugar for first-parameter functions
- **Inline assembly** with GCC-style constraints
- **Pointer arithmetic**

## Error Type Design

The error handling model is the central innovation of binar. It splits based on where an interface appears:

| Position | Strategy | Mechanism |
|----------|----------|-----------|
| Interface as **parameter** | Monomorphization | Compiler generates specialized versions for each concrete type |
| Interface as **return** | **Inline** | Function body is inlined at the call site; the function is never compiled as a standalone LLVM function |

This means:

```binar
type Myerr { code int }

fn get_error() error {
    return Myerr{code: 42}
}

fn caller() int {
    get_error() raise    // body of get_error is inlined here
    return 0             // only reached if no error
}
```

The `error` type is itself an interface. Any struct can be returned as an error value. At the call site, the compiler inlines the function body and checks the return value — if non-nil, it propagates via `raise`.

### Raise Syntax Rules

The compiler enforces correct raise syntax:

- **Single error-returning function** — raise must be standalone:
  ```binar
  get_error() raise              // OK
  err := get_error() raise       // ERROR: must be standalone
  ```

- **Multi-return (T, error)** — raise must use binding form:
  ```binar
  x := div(10, 2) raise         // OK: extracts the int value
  div(10, 2) raise              // ERROR: must use binding form
  ```

### Recursion Constraints

Since interface-returning functions are inlined, they cannot be recursive without special handling:

- **Tail recursion** — automatically transformed to a `while(true)` loop with temporary variables
- **Non-tail recursion** — compile error (would cause infinite inlining)
- **Mutual recursion** — compile error (cycle detection via call graph analysis)

```binar
// Tail recursion — compiler transforms to a loop
fn factorial(n int, acc int) error {
    if n <= 0 { return acc }
    return factorial(n - 1, acc * n)  // tail call → while loop
}

// Non-tail recursion — compile error
fn f(n int) error {
    return f(n - 1) + 1   // ERROR: non-tail recursive call
}
```

## Syntax Examples

### Variables and Functions

```binar
fn add(a int, b int) int {
    return a + b
}

fn main() int {
    x := 10           // type inferred
    y int = 20        // explicit type
    z := add(x, y)
    return z
}
```

### Structs and Methods

```binar
type Vec2 {
    X float
    Y float
}

// Method on *Vec2 (dot notation sugar)
fn Length(v *Vec2) float {
    return (v.X * v.X + v.Y * v.Y)
}

fn main() int {
    v := Vec2{ X: 3.0, Y: 4.0 }
    len := v.Length()    // sugar for Length(&v)
    return 0
}
```

### Interfaces

Interfaces use structural typing — any type with matching methods satisfies the interface:

```binar
iface Logger {
    fn Log(Logger)
}

fn Process(l Logger) {
    l.Log()
}

type Console struct {}
fn Log(c Console) { ... }  // Console satisfies Logger automatically
```

### Error Handling

```binar
type FileErr { msg string }

fn open(name string) error {
    if name == "" {
        return FileErr{msg: "empty name"}
    }
    return nil  // success
}

fn main() int {
    open("test") raise          // propagates error
    open("") raise              // propagates FileErr
    return 0
}
```

### Defer

```binar
fn process(addr *int, val int) {
    defer store(addr, 0)        // runs last (LIFO)
    defer store(addr, val)      // runs first
}

fn process_or_fail(flag *int) {
    defer store(flag, 99)       // runs even on raise
    get_error() raise
}
```

### Modules

```
mypkg/
  binar.mod       # module mypkg
  math.binar      # fn Add(a, b int) int { ... }
  util.binar      # import { math }
```

```binar
import { math } from "mypkg/math"
result := math.Add(7, 8)
```

### Constants

```binar
const N = 100
const GRID_SIZE = 4

type Grid { cells [GRID_SIZE][GRID_SIZE]int }
```

### Inline Assembly

```binar
fn syscall1(num int, arg1 int) int {
    asm volatile("syscall"
        : "={rax}" (result)
        : "{rax}" (num), "{rdi}" (arg1)
        : "rcx", "r11")
    return result
}
```

## Building

### Prerequisites

- **LLVM 21** (tested with 21.1.8)
- **CMake** 3.20+
- **C++20** compiler (GCC 15+ or Clang 18+)
- **ZLIB**

### Build

```bash
mkdir build && cd build
cmake ..
make
```

The `binar` binary will be in `build/binar`.

### Usage

```
binar [options] <input.binar>
  -o <file>    Output file (default: output.o)
  -ir          Print LLVM IR instead of compiling
  -tokens      Print tokens and exit
  -ast         Print AST and exit
```

### Compile and Run

```bash
# Compile
binar -o program.o hello.binar

# Link (static, no libc)
gcc -nostartfiles -o program program.o

# Run
./program
echo $?   # exit code
```

## Testing

```bash
bash tests/run_tests.sh
```

The test runner compiles each `.binar` file, links it, and checks the exit code. Tests use two conventions:

- `// exit: <N>` — expected runtime exit code (default: 0)
- `// error: <msg>` — expected compile error (test passes if compilation fails)

**Results**: 126/131 passing. The 5 failures are pre-existing interface compilation issues (tests 103, 105-107, 109).

## Type System

| Category | Types |
|----------|-------|
| Primitives | `int`, `float`, `bool`, `string`, `char`, `error` |
| Fixed-width | `u8`, `u16`, `u32`, `u64`, `i8`, `i16`, `i32`, `i64` |
| Pointers | `*int`, `*MyType` |
| Arrays | `[5]int`, `[N][N]float` |
| Slices | `[]int`, `[]byte` |
| Structs | `type Point { X int, Y int }` |
| Interfaces | `iface Reader { fn Read(Reader) int }` |

## Known Limitations

- **Linux x86_64 only** — no cross-compilation, no Windows/macOS
- **No libc** — uses raw syscalls via inline assembly
- **`defer` is function-scoped**, not block-scoped
- **No proper string type** — currently char arrays
- **No slices with length/capacity** — basic slice support only
- **5 interface tests failing** — monomorphization edge cases
- **No standard library** beyond basic `fmt` and `mem` packages

## Roadmap

- Block-scoped `defer`
- Enums and tagged unions / sum types
- Generics (parser support exists, codegen pending)
- String type improvements
- Slice type with length + capacity
- Range-based for loop
- Default parameters and variadic functions
- Operator overloading
- Build tooling (`binar build`, `binar run`, `binar test`)
- Dead code elimination and optimization passes
- LSP support
- WASM backend
- Windows/macOS support

## License

TBD.
