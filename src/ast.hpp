#pragma once
#include "token.hpp"
#include "value.hpp"
#include <memory>
#include <string>
#include <vector>

namespace pocketpp {

// ── Base nodes ────────────────────────────────────────────────────────────────
struct Expr { virtual ~Expr() = default; };
struct Stmt { virtual ~Stmt() = default; };
using ExprPtr = std::shared_ptr<Expr>;
using StmtPtr = std::shared_ptr<Stmt>;

// ── Expressions ───────────────────────────────────────────────────────────────
struct LiteralExpr  : Expr { Value value; explicit LiteralExpr(Value v): value(std::move(v)){} };
struct GroupingExpr : Expr { ExprPtr expr; explicit GroupingExpr(ExprPtr e): expr(std::move(e)){} };
struct VariableExpr : Expr { Token name; explicit VariableExpr(Token t): name(std::move(t)){} };
struct UnaryExpr    : Expr { Token op; ExprPtr right; UnaryExpr(Token op, ExprPtr r): op(std::move(op)), right(std::move(r)){} };
struct BinaryExpr   : Expr { ExprPtr left; Token op; ExprPtr right;
    BinaryExpr(ExprPtr l, Token op, ExprPtr r): left(std::move(l)), op(std::move(op)), right(std::move(r)){} };
struct LogicalExpr  : Expr { ExprPtr left; Token op; ExprPtr right;
    LogicalExpr(ExprPtr l, Token op, ExprPtr r): left(std::move(l)), op(std::move(op)), right(std::move(r)){} };
struct AssignExpr   : Expr { Token name; Token op; ExprPtr value;
    AssignExpr(Token n, Token op, ExprPtr v): name(std::move(n)), op(std::move(op)), value(std::move(v)){} };
// a[b] get
struct IndexExpr    : Expr { ExprPtr obj; ExprPtr idx; Token bracket;
    IndexExpr(ExprPtr o, ExprPtr i, Token b): obj(std::move(o)), idx(std::move(i)), bracket(std::move(b)){} };
// a[b] = c
struct IndexAssignExpr : Expr { ExprPtr obj; ExprPtr idx; Token op; ExprPtr val;
    IndexAssignExpr(ExprPtr o, ExprPtr i, Token op, ExprPtr v): obj(std::move(o)), idx(std::move(i)), op(std::move(op)), val(std::move(v)){} };
// a.b get
struct GetExpr      : Expr { ExprPtr obj; Token name;
    GetExpr(ExprPtr o, Token n): obj(std::move(o)), name(std::move(n)){} };
// a.b = c  or  a.b op= c
struct SetExpr      : Expr { ExprPtr obj; Token name; Token op; ExprPtr val;
    SetExpr(ExprPtr o, Token n, Token op, ExprPtr v): obj(std::move(o)), name(std::move(n)), op(std::move(op)), val(std::move(v)){} };
struct CallExpr     : Expr { ExprPtr callee; std::vector<ExprPtr> args; Token paren;
    CallExpr(ExprPtr c, std::vector<ExprPtr> a, Token p): callee(std::move(c)), args(std::move(a)), paren(std::move(p)){} };
struct FnExpr       : Expr { std::vector<std::string> params; std::vector<StmtPtr> body;
    FnExpr(std::vector<std::string> p, std::vector<StmtPtr> b): params(std::move(p)), body(std::move(b)){} };
struct ListExpr     : Expr { std::vector<ExprPtr> elements;
    explicit ListExpr(std::vector<ExprPtr> e): elements(std::move(e)){} };
struct MapExpr      : Expr { std::vector<std::pair<ExprPtr,ExprPtr>> pairs;
    explicit MapExpr(std::vector<std::pair<ExprPtr,ExprPtr>> p): pairs(std::move(p)){} };
// super() or super.name()
struct SuperExpr    : Expr { Token keyword; std::string method; bool is_call{false};
    SuperExpr(Token kw, std::string m, bool ic): keyword(std::move(kw)), method(std::move(m)), is_call(ic){} };
struct SelfExpr     : Expr { Token keyword; explicit SelfExpr(Token kw): keyword(std::move(kw)){} };
// is operator: expr is ClassName
struct IsExpr       : Expr { ExprPtr obj; ExprPtr cls;
    IsExpr(ExprPtr o, ExprPtr c): obj(std::move(o)), cls(std::move(c)){} };
// in operator: expr in expr
struct InExpr       : Expr { ExprPtr needle; ExprPtr haystack; bool negated{false};
    InExpr(ExprPtr n, ExprPtr h, bool neg=false): needle(std::move(n)), haystack(std::move(h)), negated(neg){} };
struct YieldExpr    : Expr { ExprPtr value; // nullable
    explicit YieldExpr(ExprPtr v): value(std::move(v)){} };
// string interpolation parts already handled at lex/parse time → StringConcatExpr
struct StringConcatExpr : Expr { std::vector<ExprPtr> parts;
    explicit StringConcatExpr(std::vector<ExprPtr> p): parts(std::move(p)){} };
// Not operator (unary keyword)
// Already handled via UnaryExpr with a pseudo-token

// ── Statements ────────────────────────────────────────────────────────────────
struct ExprStmt     : Stmt { ExprPtr expr; explicit ExprStmt(ExprPtr e): expr(std::move(e)){} };
struct ReturnStmt   : Stmt { ExprPtr value; // nullable
    explicit ReturnStmt(ExprPtr v): value(std::move(v)){} };
struct BreakStmt    : Stmt {};
struct BlockStmt    : Stmt { std::vector<StmtPtr> stmts;
    explicit BlockStmt(std::vector<StmtPtr> s): stmts(std::move(s)){} };
struct IfStmt       : Stmt {
    struct Branch { ExprPtr cond; std::vector<StmtPtr> body; };
    std::vector<Branch> branches; // if + elif branches
    std::vector<StmtPtr> else_body; // may be empty
};
struct WhileStmt    : Stmt { ExprPtr cond; std::vector<StmtPtr> body; };
struct ForStmt      : Stmt { std::string var; ExprPtr iter; std::vector<StmtPtr> body; };
struct FuncStmt     : Stmt { std::string name; std::vector<std::string> params; std::vector<StmtPtr> body; };
struct ClassStmt    : Stmt {
    std::string name;
    ExprPtr parent; // nullable
    std::vector<StmtPtr> methods;
    std::string docs;
};
struct ImportStmt   : Stmt {
    // import a.b.c as alias   OR   from a.b.c import x, y as z, ...
    bool is_from{false};
    std::string module_path; // "a.b.c"
    std::string alias;       // for "import x as alias" or the module name
    struct NameAlias { std::string name; std::string alias; };
    std::vector<NameAlias> names; // for "from x import a as b, c"
    // for "import a, b as c" - multiple modules
    std::vector<std::pair<std::string,std::string>> extra_imports;
};

} // namespace pocketpp
