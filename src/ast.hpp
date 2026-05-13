#pragma once

#include "token.hpp"
#include "value.hpp"

#include <memory>
#include <string>
#include <vector>

namespace pocketpp {

// ──────────────────────────────────────────────
// Base expression node
// ──────────────────────────────────────────────

struct Expr {
  virtual ~Expr() = default;
};

using ExprPtr = std::shared_ptr<Expr>;

struct LiteralExpr final : Expr {
  explicit LiteralExpr(Value value) : value(std::move(value)) {}
  Value value;
};

struct GroupingExpr final : Expr {
  explicit GroupingExpr(ExprPtr expression) : expression(std::move(expression)) {}
  ExprPtr expression;
};

struct UnaryExpr final : Expr {
  UnaryExpr(Token op, ExprPtr right) : op(std::move(op)), right(std::move(right)) {}
  Token op;
  ExprPtr right;
};

struct BinaryExpr final : Expr {
  BinaryExpr(ExprPtr left, Token op, ExprPtr right)
      : left(std::move(left)), op(std::move(op)), right(std::move(right)) {}
  ExprPtr left;
  Token op;
  ExprPtr right;
};

struct VariableExpr final : Expr {
  explicit VariableExpr(Token name) : name(std::move(name)) {}
  Token name;
};

struct AssignExpr final : Expr {
  AssignExpr(Token name, ExprPtr value) : name(std::move(name)), value(std::move(value)) {}
  Token name;
  ExprPtr value;
};

struct LogicalExpr final : Expr {
  LogicalExpr(ExprPtr left, Token op, ExprPtr right)
      : left(std::move(left)), op(std::move(op)), right(std::move(right)) {}
  ExprPtr left;
  Token op;
  ExprPtr right;
};

struct CallExpr final : Expr {
  CallExpr(ExprPtr callee, Token paren, std::vector<ExprPtr> arguments)
      : callee(std::move(callee)), paren(std::move(paren)), arguments(std::move(arguments)) {}
  ExprPtr callee;
  Token paren;
  std::vector<ExprPtr> arguments;
};

// ──────────────────────────────────────────────
// Base statement node
// ──────────────────────────────────────────────

struct Stmt {
  virtual ~Stmt() = default;
};

using StmtPtr = std::shared_ptr<Stmt>;

struct ExpressionStmt final : Stmt {
  explicit ExpressionStmt(ExprPtr expression) : expression(std::move(expression)) {}
  ExprPtr expression;
};

struct PrintStmt final : Stmt {
  explicit PrintStmt(ExprPtr expression) : expression(std::move(expression)) {}
  ExprPtr expression;
};

struct VarStmt final : Stmt {
  VarStmt(Token name, ExprPtr initializer) : name(std::move(name)), initializer(std::move(initializer)) {}
  Token name;
  ExprPtr initializer;
};

struct BlockStmt final : Stmt {
  explicit BlockStmt(std::vector<StmtPtr> statements) : statements(std::move(statements)) {}
  std::vector<StmtPtr> statements;
};

struct IfStmt final : Stmt {
  IfStmt(ExprPtr condition, StmtPtr then_branch, StmtPtr else_branch)
      : condition(std::move(condition)),
        then_branch(std::move(then_branch)),
        else_branch(std::move(else_branch)) {}
  ExprPtr condition;
  StmtPtr then_branch;
  StmtPtr else_branch;
};

struct WhileStmt final : Stmt {
  WhileStmt(ExprPtr condition, StmtPtr body)
      : condition(std::move(condition)), body(std::move(body)) {}
  ExprPtr condition;
  StmtPtr body;
};

struct FunctionStmt final : Stmt {
  FunctionStmt(Token name, std::vector<Token> params, std::vector<StmtPtr> body)
      : name(std::move(name)), params(std::move(params)), body(std::move(body)) {}
  Token name;
  std::vector<Token> params;
  std::vector<StmtPtr> body;
};

struct ReturnStmt final : Stmt {
  ReturnStmt(Token keyword, ExprPtr value)
      : keyword(std::move(keyword)), value(std::move(value)) {}
  Token keyword;
  ExprPtr value;
};

}  // namespace pocketpp
