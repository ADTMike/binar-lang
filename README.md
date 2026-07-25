# binar

A systems programming language with zero-cost error abstractions, compiled to native code via LLVM.

[![Platform](https://img.shields.io/badge/platform-Linux%20x86__64-blue)]()
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](./LICENSE)
[![Tests](https://img.shields.io/badge/tests-131%2F131%20passing-brightgreen)]()

> **Status**: Pre-release. The compiler and language are under active development.

---

## Hello World

```binar
fn main() {
    // exit code 0 = success
}
```

```bash
binar -o hello.o hello.binar
gcc -nostartfiles -o hello hello.o
./hello
echo $?   # prints 0
```

---

## Why binar?

binar is a statically typed, compiled language targeting native x86_64 Linux executables. The compiler is written in C++20 and uses LLVM 21 for code generation.

The core design principle is **zero-cost abstractions for error handling**: functions that return `error` (or any interface) are inlined at every call site at compile time. No vtable lookups, no fat pointers, no hidden allocations. The compiler generates the same code you would write by hand.

---

## Features

- **Static typing** with structs, interfaces, pointers, arrays, slices, and fixed-width integers
- **Interface system** with Go-like structural typing (no `implements` keyword)
- **Zero-cost error abstraction** -- interface-returning functions are inlined, not called
- **`raise` keyword** for ergonomic error propagation
- **`defer`** for cleanup with LIFO ordering
- **Module system** with per-package visibility (uppercase = exported)
- **Monomorphization** for interface parameters
- **Method syntax** -- dot notation as sugar for first-parameter functions
- **Inline assembly** with GCC-style constraints
- **Pointer arithmetic**

---

## Quick Start

### Prerequisites

- LLVM 21 (tested with 21.1.8)
- CMake 3.20+
- C++20 compiler (GCC 15+ or Clang 18+)
- zlib

### Building from Source

```bash
git clone https://github.com/yourname/binar-lang.git
cd binar-lang
mkdir build && cd build
cmake ..
make
```

The `binar` binary will be at `build/binar`.

### Usage

```
binar [options] <input.binar>
  -o <file>    Output file (default: output.o)
  -ir          Print LLVM IR instead of compiling
  -tokens      Print tokens and exit
  -ast         Print AST and exit
  --target <triple>  Target triple (e.g., x86_64-unknown-linux-gnu)
  --int-width <bits>  Integer width in bits (default: 64)
  --print-config  Print default config and exit
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

---

## Language Tour

### Variables and Functions

```binar
fn add(a int, b int) int {
    return a + b
}

fn main() {
    x := 10            // type inferred
    var y int = 20     // explicit type
    z := add(x, y)     // z = 30
}
```

### Return Types

Functions support exactly four return forms:

| Form | Syntax | Example |
|------|--------|---------|
| No return | `fn foo() { }` | `fn log(msg string) { }` |
| Single value | `fn foo() T { }` | `fn add(a int, b int) int { }` |
| Single error | `fn foo() error { }` | `fn read(path string) error { }` |
| Value + error | `fn foo() T, error { }` | `fn parse(s string) int, error { }` |

Multi-return uses a comma with no parentheses. The second type must always be `error`:

```binar
fn divmod(a int, b int) int, error {
    if b == 0 {
        return 0, Myerr{msg: "division by zero"}
    }
    return a / b, nil
}

fn main() {
    q, err := divmod(10, 3)
    if err != nil {
        os.Exit(1)
    }
}
```

### Structs and Methods

```binar
type Vec2 {
    X int
    Y int
}

// Method on *Vec2 (dot notation sugar)
fn Length(v *Vec2) int {
    return (v.X * v.X + v.Y * v.Y)
}

fn main() {
    v := Vec2{ X: 3, Y: 4 }
    len := v.Length()    // sugar for Length(&v)
}
```

### Error Handling

binar splits error handling based on where an interface appears:

| Position | Strategy | Mechanism |
|----------|----------|-----------|
| Interface as **parameter** | Monomorphization | Compiler generates specialized versions for each concrete type |
| Interface as **return** | **Inline** | Function body is inlined at the call site |

**Propagation** -- use `raise` to bubble errors up the call stack:

```binar
type FileErr { msg string }

fn Error(e FileErr) string {
    return "file error"
}

fn open(name string) error {
    if name == "" {
        return FileErr{msg: "empty name"}
    }
    return nil
}

fn caller() error {
    open("") raise          // propagates error to caller
    return nil              // only reached if no error
}
```

**Handling** -- capture and inspect the error value:

```binar
fn handler() {
    err := open("missing")
    if err != nil {
        os.Exit(1)
    }
}
```

**Defer** -- cleanup runs even when `raise` triggers a return:

```binar
fn process(name string) error {
    defer cleanup()
    open(name) raise
    return nil
}
```

### Interfaces

Interfaces use structural typing -- any type with matching methods satisfies the interface automatically:

```binar
iface Logger {
    fn Log(Logger)
}

type Console {}

fn Log(c Console) { ... }  // Console satisfies Logger automatically

fn Process(l Logger) {
    l.Log()
}
```

### Module System

binar uses a Go-inspired module and import system with directory-based packages.

**Module declaration** -- every project that uses imports needs a `binar.mod`:

```
module myproject
require "dep" => "../dep"
```

**Imports:**

```binar
// Cross-package
import { math } from "myproject/math"
result := math.Add(7, 8)

// Same-package (no from)
import { a }
a.helper()
```

**Visibility** -- Go-style, based on first letter:

- Uppercase (`Add`, `Process`) -- exported, callable from any package
- Lowercase (`helper`, `internal`) -- private to the same package

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

---

## Error Handling Deep Dive

### Raise Syntax Rules

The compiler enforces correct raise syntax:

- **Single error-returning function** -- raise must be standalone:
  ```binar
  get_error() raise              // OK
  err := get_error() raise       // ERROR: must be standalone
  ```

- **Multi-return (T, error)** -- raise must use binding form:
  ```binar
  x := div(10, 2) raise         // OK: extracts the int value
  div(10, 2) raise              // ERROR: must use binding form
  ```

### Recursion Constraints

Since interface-returning functions are inlined, they cannot be recursive without special handling:

- **Tail recursion** -- automatically transformed to a `while(true)` loop
- **Non-tail recursion** -- compile error (would cause infinite inlining)
- **Mutual recursion** -- compile error (cycle detection via call graph analysis)

```binar
// Tail recursion -- compiler transforms to a loop
fn factorial(n int, acc int) error {
    if n <= 0 { return acc }
    return factorial(n - 1, acc * n)  // tail call -> while loop
}

// Non-tail recursion -- compile error
fn f(n int) error {
    return f(n - 1) + 1   // ERROR: non-tail recursive call
}
```

---

## Module System Deep Dive

### Multi-File Packages

A directory is a package. All `.binar` files in the same directory share a namespace:

```
mypkg/
    a.binar      # fn Add(a int, b int) int { ... }
    b.binar      # import { a }
                 # fn Calc(x int) int { return a.Add(x, 1) }
```

```binar
// main.binar
import { b } from "myproject/mypkg"
b.Calc(5)
```

### Resolution Algorithm

1. Walk up from the input file looking for `binar.mod` -> module root
2. For each import path, resolve in order: required dependencies (longest prefix match) -> self-module -> `$BINAR_HOME/std/`
3. Files are compiled **lazily** -- only when their exports are actually called
4. Each file is parsed and compiled at most once (deduplication)

---

## Type System

| Category | Types |
|----------|-------|
| Primitives | `int`, `float`, `bool`, `string`, `char`, `error` |
| Fixed-width | `u8`, `u16`, `u32`, `u64`, `i8`, `i16`, `i32`, `i64` |
| Pointers | `*int`, `*MyType` |
| Arrays | `[5]int`, `[N][N]int` |
| Slices | `[]int`, `[]byte` |
| Structs | `type Point { X int; Y int }` |
| Interfaces | `iface Reader { fn Read(Reader) int }` |

---

## Testing

```bash
bash tests/run_tests.sh
```

The test runner compiles each `.binar` file, links it, and checks the exit code:

- `// exit: <N>` -- expected runtime exit code (default: 0)
- `// error: <msg>` -- expected compile error (test passes if compilation fails)

**Results**: 131/131 tests passing.

---

## Known Limitations

- **Linux x86_64 only** -- no cross-compilation, no Windows/macOS
- **No libc** -- uses raw syscalls via inline assembly
- **`defer` is function-scoped**, not block-scoped
- **No proper string type** -- currently char arrays
- **No slices with length/capacity** -- basic slice support only
- **No standard library** beyond basic `fmt` and `mem` packages

---

## License

[MIT](./LICENSE)
