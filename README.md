# binar

A compiled systems language targeting native x86_64 Linux. Written in C++20, backend is LLVM 21.

No libc.

---

## Building

```bash
# requires: LLVM 21, CMake 3.20+, C++20 compiler
mkdir build && cd build
cmake ..
make
```

## Running

```bash
binar -o program.o program.binar
gcc -nostartfiles -o program program.o
./program
echo $?
```

---

## Examples

### Hello World

```binar
import { os } from "os"
import { fmt } from "fmt"

fn main() {
    fmt.Println("hello world")
    os.Exit(0)
}
```

### Variables

```binar
fn main() {
    x := 10
    var y int = 20
    z := x + y   // 30
}
```

Global variables:

```binar
x := 10            // unexported (lowercase first letter)
Abc := 10          // exported (uppercase first letter)
_abc := 10         // unexported (_ prefix)

y int              // type annotation only
z int = 10         // type annotation + initializer
```

### Constants

ALL_CAPS names are constants (inlined at compile time):

```binar
RED 10
BLUE 20
_GREEN 30          // unexported
```

Iota auto-increment:

```binar
RED iota           // 0
BLUE               // 1
YELLOW             // 2
```

### Functions

```binar
fn add(a int, b int) int {
    return a + b
}

fn divmod(a int, b int) int, error {
    if b == 0 {
        return 0, Myerr{msg: "division by zero"}
    }
    return a / b, nil
}
```

### Structs

```binar
type Vec2 {
    X int
    Y int
}

fn Length(v *Vec2) int {
    return v.X * v.X + v.Y * v.Y
}

fn main() {
    v := Vec2{ X: 3, Y: 4 }
    len := v.Length()   // sugar for Length(&v)
}
```

### Error Handling

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

// raise propagates errors up the call stack
fn caller() error {
    open("") raise
    return nil
}

// defer runs cleanup even when raise triggers a return
fn process(name string) error {
    defer cleanup()
    open(name) raise
    return nil
}
```

### Structural Types

`~TypeName` accepts any type with matching methods. Methods are inferred from usage in the function body.

```binar
type ZapLogger { X int }
type ConsoleLogger { X int }

fn GetVal(l ZapLogger) int { return l.X }
fn GetVal(l ConsoleLogger) int { return l.X }

fn Process(l ~ZapLogger) int {
    return l.GetVal()
}

fn main() {
    z := ZapLogger{X: 10}
    c := ConsoleLogger{X: 20}
    Process(z)    // OK: ZapLogger has GetVal
    Process(c)    // OK: ConsoleLogger has GetVal
}
```

### Generics

```binar
fn max[T](a T, b T) T {
    if a > b { return a }
    return b
}

type Pair[T] {
    first T
    second T
}

fn main() {
    check(20, max(10, 20))              // type inference
    check(99, max[int](99, 1))          // explicit type arg
    p := Pair[int]{ first: 1, second: 2 }
}
```

### Modules

```binar
// cross-package import
import { math } from "myproject/math"
result := math.Add(7, 8)

// same-package import
import { a }
a.helper()
```

Visibility: `_` prefix = unexported, uppercase first letter = exported, lowercase first letter = unexported.

Projects with imports need a `binar.mod`:
```
module myproject
require "dep" => "../dep"
```

Standard library (`os`, `fmt`) is always available without `binar.mod`.

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

## Types

| | |
|---|---|
| `int`, `float`, `bool`, `string`, `char`, `error` | primitives |
| `u8`..`u64`, `i8`..`i64` | fixed-width integers |
| `*int`, `*MyType` | pointers |
| `[5]int` | arrays |
| `[]int` | slices |
| `type Point { X int; Y int }` | structs |
| `~TypeName` | structural type |

`string` is `{ptr, len}` — a two-word struct, like Go. Use `len(s)` to get its length.

## Return Types

Only four forms:

```binar
fn foo() { }                     // no return
fn foo() int { }                 // single value
fn foo() error { }               // single error
fn foo() int, error { }          // value + error
```

## CLI

```
binar [options] <input.binar>
  -o <file>              Output file (default: output.o)
  -ir                    Print LLVM IR
  -tokens                Print tokens
  -ast                   Print AST
  --target <triple>      e.g. x86_64-unknown-linux-gnu
  --int-width <bits>     Integer width (default: 64)
  --print-config         Print config and exit
```

## Standard Library

```binar
import { os } from "os"
os.Exit(0)

import { fmt } from "fmt"
fmt.Print("hello")      // no newline
fmt.Println("hello")    // with newline
```

---

## Tests

```bash
bash tests/run_tests.sh
```

48/48 passing.
