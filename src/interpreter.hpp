#pragma once

#include "ast.hpp"
#include "callable.hpp"
#include "environment.hpp"
#include "errors.hpp"
#include "value.hpp"

#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace pocketpp {

// ──────────────────────────────────────────────
// Signal used to unwind the call stack on return
// ──────────────────────────────────────────────

class ReturnSignal : public std::runtime_error {
 public:
  explicit ReturnSignal(Value value) : std::runtime_error("return"), value(std::move(value)) {}

  Value value;
};

// ──────────────────────────────────────────────
// Interpreter
// ──────────────────────────────────────────────

class Interpreter {
 public:
  Interpreter();

  void interpret(const std::vector<StmtPtr>& statements);
  std::string output() const;
  void execute_block(const std::vector<StmtPtr>& statements,
                     const std::shared_ptr<Environment>& environment);

 private:
  static bool is_truthy(const Value& value);
  static bool is_equal(const Value& a, const Value& b);
  static std::string stringify(const Value& value);
  static double expect_number(const Value& value, const std::string& message);

  Value evaluate(const ExprPtr& expr);
  void execute(const StmtPtr& statement);

  std::shared_ptr<Environment> globals_;
  std::shared_ptr<Environment> environment_;
  std::ostringstream output_;

  friend class UserFunction;
};

// ──────────────────────────────────────────────
// User-defined function callable
// ──────────────────────────────────────────────

class UserFunction final : public Callable {
 public:
  UserFunction(std::shared_ptr<FunctionStmt> declaration, std::shared_ptr<Environment> closure);

  int arity() const override;
  Value call(Interpreter& interpreter, const std::vector<Value>& args) override;
  std::string to_string() const override;

 private:
  std::shared_ptr<FunctionStmt> declaration_;
  std::shared_ptr<Environment> closure_;
};

// ──────────────────────────────────────────────
// Native callable: clock()
// ──────────────────────────────────────────────

class ClockNative final : public Callable {
 public:
  int arity() const override;
  Value call(Interpreter& interpreter, const std::vector<Value>& args) override;
  std::string to_string() const override;
};

}  // namespace pocketpp
