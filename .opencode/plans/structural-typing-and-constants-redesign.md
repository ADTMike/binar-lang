# Implementation Plan: ~ Structural Typing + Constants Redesign

## Language Design Summary

### Export Convention (`_` prefix, uppercase first letter)

```
_name   → unexported (always, _ prefix wins)
Name    → exported (first letter uppercase, no _)
name    → unexported (first letter lowercase, no _)
NAME    → constant only (ALL_CAPS), export follows same rules
```

Examples:
```binar
RED 10       // exported constant (ALL_CAPS, first letter uppercase)
_RED 10      // unexported constant (_ prefix)
Process()    // exported function (first letter uppercase)
_process()   // unexported function (_ prefix)
Abc := 10    // exported variable (first letter uppercase)
_abc := 10   // unexported variable (_ prefix)
x := 10      // unexported variable (first letter lowercase)

type Logger { }    // exported type
type _Logger { }   // unexported type
type logger { }    // unexported type (first letter lowercase)

type Foo {
    Bar int        // exported field
    _baz int       // unexported field
}
```

Rules:
- `_` prefix = unexported (always, for any name type)
- No `_` prefix + uppercase first letter = exported
- No `_` prefix + lowercase first letter = unexported
- ALL_CAPS names = constants only (not functions, types, variables)

### Constants (ALL_CAPS, no `const` keyword)

```binar
// Iota auto-increment:
RED iota        // exported, 0
BLUE            // exported, 1
YELLO           // exported, 2
_GREEN          // unexported, 3

// Explicit values (space-separated, no =):
RED     10      // exported
_BLUE   20      // unexported

// Iota group with mix:
_RED iota       // unexported, 0
_BLUE           // unexported, 1
YELLO           // exported, 2
GREEN           // exported, 3
```

Rules:
- ALL_CAPS names = constants (inlined at compile time, no type annotation)
- `iota` on same line starts/increments auto-counter (begins at 0)
- Bare name (no value) = implicit next iota value
- Export follows `_` prefix / uppercase first letter convention
- No `=` sign for constant values
- No `pub` keyword needed

### Global Variables (camelCase/PascalCase)

```binar
x := 10          // unexported (first letter lowercase)
y int            // unexported (first letter lowercase)
z int = 10       // unexported

Abc := 10        // exported (first letter uppercase)
Abc int          // exported
Abc int = 10     // exported

_abc := 10       // unexported (_ prefix)
_abc int         // unexported
```

### Structural Types (`~TypeName`)

```binar
type ZapLogger { X int }
type ConsoleLogger { X int }

fn GetVal(l ZapLogger) { }
fn GetVal(l ConsoleLogger) { }

fn Process(l ~ZapLogger) int {
    l.GetVal()
}

fn main() {
    var l ~ZapLogger
    l = ZapLogger{X: 3}
    l = ConsoleLogger{X: 4}
    Process(l)
}
```

Rules:
- `~TypeName` = structural type (any type with required methods)
- `~*TypeName` = pointer receiver structural type
- Parameters, variables, return types all support `~`
- Only methods actually used inside the function body are required
- Tagged union dispatch at runtime

### Removed Features
- `const` keyword → ALL_CAPS convention
- `port` keyword → replaced by `~`
- `adapt` keyword → replaced by `~`
- `pub` keyword → replaced by `_` prefix convention
- Enum type → `const` + `~` covers all use cases

---

## Phase 1: Lexer Changes (`src/token.h`, `src/lexer.cpp`)

### token.h
1. Remove `KW_PORT` (line 38)
2. Remove `KW_ADAPT` (line 39)
3. Remove `KW_CONST` (line 20)
4. Add `KW_IOTA` (after `KW_RAISE`)
5. Do NOT add `KW_PUB` (using `_` prefix convention instead)

### lexer.cpp
1. Remove `"port"` → `KW_PORT` from keywords map
2. Remove `"adapt"` → `KW_ADAPT` from keywords map
3. Remove `"const"` → `KW_CONST` from keywords map
4. Add `"iota"` → `KW_IOTA` to keywords map
5. Update `token_type_name()`:
   - Remove `case TokenType::KW_CONST: return "const";`
   - Remove `case TokenType::KW_PORT: return "port";`
   - Remove `case TokenType::KW_ADAPT: return "adapt";`
   - Add `case TokenType::KW_IOTA: return "iota";`

---

## Phase 2: AST Changes (`src/ast.h`)

### TypeKind enum
- Add `STRUCTURAL` after `TYPE_PARAM`
- Remove `PORT`

### New structs
```cpp
struct ConstantDecl {
    std::string name;
    ExprPtr value;       // null for bare iota
    bool is_iota;        // true if this uses iota
    bool is_exported;    // true if pub
};

struct GlobalVarDecl {
    std::string name;
    TypePtr type;        // optional explicit type
    ExprPtr value;       // optional initializer
    bool is_exported;    // true if starts with uppercase
};
```

### DeclKind enum
- Remove `PORT`
- Remove `CONST`
- Add `CONSTANT`
- Add `GLOBAL_VAR`

### Decl struct
- Remove `port_decl`
- Remove `const_decl`
- Add `constant_decl`
- Add `global_var_decl`

### Remove
- `PortDecl` struct
- `PortMethod` struct
- `adapt_port` from `TypeDecl`
- `type` from `ConstDecl`

---

## Phase 3: Parser Changes (`src/parser.cpp`)

### Parse `~TypeName` in `parse_type()`
After pointer/slice/array/fn parsing, before named type:
```cpp
if (check(TokenType::TILDE)) {
    advance(); // consume '~'
    auto type = std::make_unique<TypeAnnotation>();
    type->kind = TypeKind::STRUCTURAL;
    if (match(TokenType::STAR)) {
        auto ptr = std::make_unique<TypeAnnotation>();
        ptr->kind = TypeKind::POINTER;
        ptr->inner = std::make_unique<TypeAnnotation>();
        ptr->inner->kind = TypeKind::STRUCTURAL;
        ptr->inner->name = expect(TokenType::IDENT, "expected type name after '~*'").value;
        type->inner = std::move(ptr);
    } else {
        type->name = expect(TokenType::IDENT, "expected type name after '~'").value;
    }
    return type;
}
```

### Remove `parse_port_decl()` entirely

### Remove `KW_PORT` and `KW_CONST` from `parse_decl()`

### Remove `adapt` from `parse_type_decl()` (lines 203-206)

### New `parse_top_level_decl()` — replaces `parse_decl()` for top-level
```cpp
Decl Parser::parse_top_level_decl() {
    if (check(TokenType::KW_FN)) return parse_fn_decl();
    if (check(TokenType::KW_TYPE)) return parse_type_decl();
    if (check(TokenType::KW_IMPORT)) return parse_import();
    if (check(TokenType::IDENT) && is_all_caps(peek().value)) return parse_constant_decl();
    if (check(TokenType::IDENT) || check_next(TokenType::COLON_ASSIGN)) return parse_global_var_decl();
    error("unexpected token at top level");
}
```

### New `parse_constant_decl()`
```cpp
Decl Parser::parse_constant_decl() {
    Decl decl;
    decl.kind = DeclKind::CONSTANT;
    Token name_tok = advance(); // ALL_CAPS name
    decl.constant_decl.name = name_tok.value;
    decl.constant_decl.is_exported = !name_tok.value.empty() && name_tok.value[0] != '_';
    
    if (check(TokenType::KW_IOTA) || (check(TokenType::IDENT) && peek().value == "iota")) {
        advance(); // consume 'iota'
        decl.constant_decl.is_iota = true;
        decl.constant_decl.value = make_iota_literal(current_iota_index_++);
    } else {
        // Explicit value: RED 10
        decl.constant_decl.is_iota = false;
        decl.constant_decl.value = parse_expr();
    }
    
    return decl;
}
```

### New `parse_global_var_decl()`
```cpp
Decl Parser::parse_global_var_decl() {
    Decl decl;
    decl.kind = DeclKind::GLOBAL_VAR;
    Token name = advance(); // consume name
    
    decl.global_var_decl.name = name.value;
    // Export: _ prefix = unexported, uppercase first letter = exported, lowercase = unexported
    if (!name.value.empty() && name.value[0] == '_') {
        decl.global_var_decl.is_exported = false;
    } else if (!name.value.empty() && std::isupper(name.value[0])) {
        decl.global_var_decl.is_exported = true;
    } else {
        decl.global_var_decl.is_exported = false;
    }
    
    // Check for type annotation
    if (check(TokenType::TYPE_INT) || check(TokenType::TYPE_FLOAT) || 
        check(TokenType::TYPE_BOOL) || check(TokenType::TYPE_STRING) ||
        check(TokenType::IDENT)) {
        decl.global_var_decl.type = parse_type();
    }
    
    // Check for initializer
    if (match(TokenType::ASSIGN)) {
        decl.global_var_decl.value = parse_expr();
    }
    
    return decl;
}
```

### Helper functions
```cpp
bool Parser::is_all_caps(const std::string& name) {
    if (name.empty()) return false;
    for (char c : name) {
        if (!std::isupper(c) && c != '_') return false;
    }
    return true;
}

bool Parser::is_pascal_case(const std::string& name) {
    return !name.empty() && std::isupper(name[0]);
}
```

### Iota tracking
- Add `int current_iota_index_ = 0;` as parser member
- Reset to 0 when a new `iota` keyword is encountered
- `make_iota_literal(int val)` creates an INT_LIT expr with value

### Update `parse_var_decl_or_expr_stmt()`
- Add `check(TokenType::TILDE)` to the type detection check (line 843)

---

## Phase 4: Sema Changes (`src/sema.cpp`, `src/sema.h`)

### Remove
- `PortInfo` struct (sema.h)
- `check_port_decl()` method
- `ports_` member
- `port_implementations_` member
- adapt validation (sema.cpp lines 67-78)
- `PORT` case from `check_decl()`
- `check_error_port_impl()` method
- `has_method()` helper

### Add
- `CONSTANT` case to `check_decl()` — validate ALL_CAPS naming
- `GLOBAL_VAR` case to `check_decl()` — validate type/initializer
- `STRUCTURAL` case to `resolve_type()` — verify reference type is defined struct
- Validation: when function param has `~TypeName`, verify `TypeName` is defined struct

---

## Phase 5: Codegen Changes (`src/codegen.cpp`)

### Rename infrastructure
| Old | New |
|---|---|
| `port_names_` | `structural_names_` |
| `port_tu_types_` | `structural_tu_types_` |
| `port_impls_reg_` | `structural_impls_reg_` |
| `port_methods_` | `structural_methods_` |
| `port_param_types_` | `structural_param_types_` |

### Rewrite `discover_port_implementations()` → `discover_structural_implementations()`
- Remove `adapt`-based registration (lines 419-429)
- Keep implicit function-matching loop (lines 432-461)
- Use `structural_methods_` instead of `port_methods_`

### Remove `collect_tu_info()` port parsing (lines 548-562)
No more PORT declarations to collect.

### Update `create_tu_types()` (line 539)
Iterate `structural_impls_reg_` instead of `port_impls_reg_`.

### Update `resolve_type()` (line 2387)
- Look up `structural_tu_types_` instead of `port_tu_types_`
- Add `TypeKind::STRUCTURAL` case

### Fix `ensure_in_tu()` (line 644)
Replace silent tag-0 fallback with error:
```cpp
tag = find_structural_tag(ref_name, concrete_name);
if (tag < 0) {
    error("type '" + concrete_name + "' does not have the methods required by '~" + ref_name + "'");
    tag = 0; // fallback for error recovery
}
```

### Update CALL handler (lines 2076-2087, 2105-2117)
Replace `port_tu_types_` with `structural_tu_types_`, `port_names_` with `structural_names_`.

### Update DOT_ACCESS handler
Replace port dispatch with structural dispatch.

### Rename `gen_port_dispatch()` → `gen_structural_dispatch()`

### Add CONSTANT case to `gen_decl()`
Treat like current CONST but from the new AST node. Store in `const_values_` map.

### Add GLOBAL_VAR case to `gen_decl()`
Create LLVM global variable with `new llvm::GlobalVariable()`, handle initialization.

---

## Phase 6: Test Rewriting

### Port tests (103-109) → ~ syntax
- Remove `port` declarations
- Remove `adapt` declarations
- Change `fn Process(l Logger)` → `fn Process(l ~ZapLogger)`

### Adapt tests (132-136) → ~ syntax
Convert all `adapt` usage to `~` structural types.

### 12_ports.binar
Rewrite with `~` syntax.

### New tests to add
- `~` variable declaration and assignment
- `~` return types
- `~*TypeName` pointer structural
- Constants with `iota` (basic, expressions, export convention)
- `_` prefix export convention
- Global variables (exported/unexported)
- `iota` outside const context (error test)
- Sema error: type doesn't have required methods
- Sema error: ALL_CAPS name used as variable

---

## Execution Order

1. **Lexer** — add KW_IOTA, KW_PUB, remove KW_PORT/KW_ADAPT/KW_CONST
2. **AST** — add STRUCTURAL, CONSTANT, GLOBAL_VAR, remove PORT/PortDecl/adapt_port
3. **Parser** — parse ~TypeName, new const syntax, new global var syntax, remove port parsing
4. **Sema** — remove port validation, add structural/const/global var validation
5. **Codegen** — rename port→structural, fix tag-0 fallback, update dispatch, add global var codegen
6. **Tests** — rewrite port tests, add new tests for iota/pub/~/global vars
7. **Cleanup** — remove dead code, verify all tests pass
