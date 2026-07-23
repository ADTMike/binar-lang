#pragma once

#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <map>
#include "token.h"

namespace binar {

// Forward declarations
struct Expr;
struct Stmt;
struct TypeAnnotation;
struct Param;
struct FnParam;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using TypePtr = std::unique_ptr<TypeAnnotation>;

// ==================== Type Annotations ====================

enum class TypeKind {
    NAMED,      // int, float, string, Vec2, etc.
    POINTER,    // *int, *Vec2
    SLICE,      // []int
    ARRAY,      // [5]int
    FUNCTION,   // fn(int, int) -> int
    INTERFACE,  // Logger (interface type annotation)
    TYPE_PARAM, // T (generic type parameter)
};

struct TypeAnnotation {
    TypeKind kind;
    std::string name;           // NAMED: type name
    TypePtr inner;              // POINTER/SLICE: inner type
    int array_size;             // ARRAY: size (or -1 for const expr)
    std::string array_size_name;// ARRAY: const name for size
    std::vector<FnParam> fn_params;   // FUNCTION: params
    std::vector<TypePtr> fn_returns;  // FUNCTION: return types

    TypeAnnotation() : kind(TypeKind::NAMED), array_size(-1) {}
};

// ==================== Expressions ====================

enum class ExprKind {
    INT_LIT,
    FLOAT_LIT,
    STRING_LIT,
    CHAR_LIT,
    BOOL_LIT,
    NIL_LIT,
    IDENT,
    BINARY,
    UNARY,
    ASSIGN,
    CALL,
    INDEX,
    SLICE,
    DOT_ACCESS,
    SIZEOF,
    STRUCT_LITERAL,
    POSTFIX_INC,
    POSTFIX_DEC,
};

enum class BinOp {
    ADD, SUB, MUL, DIV, MOD,
    EQ, NEQ, LT, GT, LTE, GTE,
    AND, OR,
    BIT_AND, BIT_OR, BIT_XOR,
    LSHIFT, RSHIFT,
};

enum class UnOp {
    NEG, NOT, BIT_NOT, DEREF, ADDR_OF,
};

struct BinaryExpr {
    BinOp op;
    ExprPtr left;
    ExprPtr right;
};

struct UnaryExpr {
    UnOp op;
    ExprPtr operand;
};

struct CallExpr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
};

struct IndexExpr {
    ExprPtr object;
    ExprPtr index;
};

struct SliceExpr {
    ExprPtr object;
    ExprPtr start;  // optional
    ExprPtr end;    // optional
};

struct DotExpr {
    ExprPtr object;
    std::string field;
};

struct AssignExpr {
    ExprPtr target;
    ExprPtr value;
    bool is_decl;  // true for :=, false for =
    bool has_type; // true when type is explicitly annotated (e.g. x int, x int = 42)
    std::string type_name;  // explicit type name when has_type is true
    TokenType assign_op;  // ASSIGN, PLUS_ASSIGN, MINUS_ASSIGN, etc.
    bool raise = false;    // true when expr is followed by 'raise'
};

struct StructFieldInit {
    std::string name;
    ExprPtr value;
};

struct StructLiteralExpr {
    std::string type_name;
    std::vector<StructFieldInit> fields;
};

struct PostfixExpr {
    ExprPtr operand;
};

struct Expr {
    ExprKind kind;
    int line;
    int column;

    // Data for each kind
    int64_t int_val;
    double float_val;
    std::string string_val;
    char char_val;
    bool bool_val;
    std::string ident;
    BinaryExpr binary;
    UnaryExpr unary;
    CallExpr call;
    IndexExpr index;
    SliceExpr slice;
    DotExpr dot;
    AssignExpr assign;
    StructLiteralExpr struct_literal;
    PostfixExpr postfix;

    Expr() : kind(ExprKind::INT_LIT), line(0), column(0),
             int_val(0), float_val(0.0), char_val(0), bool_val(false) {}
};

// ==================== Statements ====================

enum class StmtKind {
    EXPR,
    RETURN,
    IF,
    FOR,
    WHILE,
    FOR_RANGE,
    BLOCK,
    BREAK,
    CONTINUE,
    DEFER,
    SWITCH,
    ASM,
    MULTI_ASSIGN,
};

struct IfBranch {
    ExprPtr condition;
    std::vector<StmtPtr> body;
};

struct IfStmt {
    std::vector<IfBranch> branches;  // if/else if
    std::vector<StmtPtr> else_body;  // else
};

struct ForStmt {
    StmtPtr init;
    ExprPtr cond;
    ExprPtr update;
    std::vector<StmtPtr> body;
};

struct WhileStmt {
    ExprPtr condition;
    std::vector<StmtPtr> body;
};

struct ForRangeStmt {
    std::string index_name;
    std::string value_name;
    ExprPtr range_expr;
    std::vector<StmtPtr> body;
};

struct BlockStmt {
    std::vector<StmtPtr> stmts;
};

struct DeferStmt {
    ExprPtr expr;
};

struct SwitchCase {
    std::vector<ExprPtr> values;  // empty for default
    std::vector<StmtPtr> body;
    bool is_default;
};

struct SwitchStmt {
    ExprPtr value;  // optional (nil for expression switch)
    std::vector<SwitchCase> cases;
};

struct AsmOperand {
    std::string constraint;  // e.g. "={rax}", "{rax}", "r", "memory"
    std::string name;        // optional name (e.g. "ret")
    ExprPtr value;           // the expression to bind
    bool is_output;          // true for output, false for input
};

struct AsmStmt {
    std::string code;
    bool is_volatile;
    std::vector<AsmOperand> outputs;
    std::vector<AsmOperand> inputs;
    std::vector<std::string> clobbers;
};

struct MultiAssignStmt {
    std::vector<std::string> targets;  // variable names on LHS
    ExprPtr value;                      // the call expression on RHS
    bool is_decl;                       // true for :=, false for =
    std::vector<std::string> type_names; // optional explicit types (same size as targets)
};

struct Stmt {
    StmtKind kind;
    int line;
    int column;

    ExprPtr expr;           // EXPR, RETURN
    std::vector<ExprPtr> return_values; // RETURN (multi-return)
    IfStmt if_stmt;         // IF
    ForStmt for_stmt;       // FOR
    WhileStmt while_stmt;   // WHILE
    ForRangeStmt range_stmt;// FOR_RANGE
    BlockStmt block;        // BLOCK
    DeferStmt defer_stmt;   // DEFER
    SwitchStmt switch_stmt; // SWITCH
    AsmStmt asm_stmt;       // ASM
    MultiAssignStmt multi_assign; // MULTI_ASSIGN

    bool raise = false;  // true for standalone expr followed by 'raise'

    Stmt() : kind(StmtKind::EXPR), line(0), column(0) {}
};

// ==================== Top-level Declarations ====================

enum class DeclKind {
    FN,
    TYPE,
    CONST,
    IMPORT,
    IFACE,
};

struct FnParam {
    std::string name;
    TypePtr type;
};

struct InterfaceMethod {
    std::string name;
    TypePtr param_type;
    bool is_pointer;
};

struct InterfaceDecl {
    std::string name;
    std::vector<InterfaceMethod> methods;
};

struct FnDecl {
    std::string name;
    std::vector<FnParam> params;
    std::vector<TypePtr> return_types;
    std::vector<StmtPtr> body;
    bool has_body;
    std::vector<std::string> type_params;
    std::map<std::string, TypePtr> type_constraints;
    bool is_interface_returning = false;  // true if any return type is an interface (error, user iface)
};

struct FieldDecl {
    std::string name;
    TypePtr type;
};

struct TypeDecl {
    std::string name;
    std::vector<FieldDecl> fields;
};

struct ConstDecl {
    std::string name;
    ExprPtr value;
    TypePtr type;  // optional explicit type
};

struct ImportBinding {
    std::string name;
    std::string package_path;  // empty = same package
};

struct ImportBlock {
    std::vector<ImportBinding> bindings;
    bool is_from;  // true: import { x } from "pkg", false: import { x } (same package)
};

struct Decl {
    DeclKind kind;
    int line;
    int column;

    FnDecl fn;
    TypeDecl type_decl;
    ConstDecl const_decl;
    ImportBlock import_block;
    InterfaceDecl iface_decl;

    Decl() : kind(DeclKind::FN), line(0), column(0) {}
};

// ==================== Program ====================

struct Program {
    std::vector<Decl> decls;
};

// ==================== Utility ====================

std::string binop_str(BinOp op);
std::string unop_str(UnOp op);

// ==================== Clone Utilities ====================

ExprPtr clone_expr(const ExprPtr& src);
StmtPtr clone_stmt(const StmtPtr& src);

} // namespace binar
