#include "interpreter.hpp"

#include <cmath>
#include <ctime>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace pocketpp {

// ─── UserFunction ───────────────────────────────────────────────────────────

UserFunction::UserFunction(std::shared_ptr<FunctionStmt> declaration,
                           std::shared_ptr<Environment> closure)
    : declaration_(std::move(declaration)), closure_(std::move(closure)) {}

int UserFunction::arity() const { return static_cast<int>(declaration_->params.size()); }

std::string UserFunction::to_string() const { return "<fn " + declaration_->name.lexeme + ">"; }

Value UserFunction::call(Interpreter& interpreter, const std::vector<Value>& args) {
  auto environment = std::make_shared<Environment>(closure_);
  for (std::size_t i = 0; i < declaration_->params.size(); ++i) {
    environment->define(declaration_->params.at(i).lexeme, args.at(i));
  }

  try {
    interpreter.execute_block(declaration_->body, environment);
  } catch (const ReturnSignal& signal) {
    return signal.value;
  }

  return std::monostate{};
}

// ─── ClockNative ────────────────────────────────────────────────────────────

int ClockNative::arity() const { return 0; }

Value ClockNative::call(Interpreter&, const std::vector<Value>&) {
  return static_cast<double>(std::time(nullptr));
}

std::string ClockNative::to_string() const { return "<native fn>"; }

// ─── Interpreter ────────────────────────────────────────────────────────────

Interpreter::Interpreter()
    : globals_(std::make_shared<Environment>()), environment_(globals_) {
  globals_->define("clock", std::make_shared<ClockNative>());
}

void Interpreter::interpret(const std::vector<StmtPtr>& statements) {
  for (const auto& statement : statements) {
    execute(statement);
  }
}

std::string Interpreter::output() const { return output_.str(); }

void Interpreter::execute_block(const std::vector<StmtPtr>& statements,
                                const std::shared_ptr<Environment>& environment) {
  const auto previous = environment_;
  environment_ = environment;
  try {
    for (const auto& statement : statements) {
      execute(statement);
    }
  } catch (...) {
    environment_ = previous;
    throw;
  }
  environment_ = previous;
}

bool Interpreter::is_truthy(const Value& value) {
  if (std::holds_alternative<std::monostate>(value)) {
    return false;
  }
  if (const auto* b = std::get_if<bool>(&value)) {
    return *b;
  }
  return true;
}

bool Interpreter::is_equal(const Value& a, const Value& b) { return a == b; }

std::string Interpreter::stringify(const Value& value) {
  return std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return "nil";
        } else if constexpr (std::is_same_v<T, bool>) {
          return v ? "true" : "false";
        } else if constexpr (std::is_same_v<T, double>) {
          std::ostringstream oss;
          oss << v;
          auto text = oss.str();
          if (text.find('.') != std::string::npos) {
            while (!text.empty() && text.back() == '0') {
              text.pop_back();
            }
            if (!text.empty() && text.back() == '.') {
              text.pop_back();
            }
          }
          return text;
        } else if constexpr (std::is_same_v<T, std::shared_ptr<Callable>>) {
          return v ? v->to_string() : "<null callable>";
        } else {
          return v;
        }
      },
      value);
}

double Interpreter::expect_number(const Value& value, const std::string& message) {
  if (const auto* number = std::get_if<double>(&value)) {
    return *number;
  }
  throw RuntimeError(message);
}

Value Interpreter::evaluate(const ExprPtr& expr) {
  if (const auto* literal = dynamic_cast<LiteralExpr*>(expr.get())) {
    return literal->value;
  }

  if (const auto* grouping = dynamic_cast<GroupingExpr*>(expr.get())) {
    return evaluate(grouping->expression);
  }

  if (const auto* unary = dynamic_cast<UnaryExpr*>(expr.get())) {
    const auto right = evaluate(unary->right);

    if (unary->op.type == TokenType::Minus) {
      return -expect_number(right, "Operand must be a number.");
    }
    if (unary->op.type == TokenType::Bang) {
      return !is_truthy(right);
    }
  }

  if (const auto* variable = dynamic_cast<VariableExpr*>(expr.get())) {
    return environment_->get(variable->name);
  }

  if (const auto* assign = dynamic_cast<AssignExpr*>(expr.get())) {
    auto value = evaluate(assign->value);
    environment_->assign(assign->name, value);
    return value;
  }

  if (const auto* logical = dynamic_cast<LogicalExpr*>(expr.get())) {
    auto left = evaluate(logical->left);

    if (logical->op.type == TokenType::Or) {
      if (is_truthy(left)) {
        return left;
      }
    } else {
      if (!is_truthy(left)) {
        return left;
      }
    }

    return evaluate(logical->right);
  }

  if (const auto* binary = dynamic_cast<BinaryExpr*>(expr.get())) {
    auto left = evaluate(binary->left);
    auto right = evaluate(binary->right);

    switch (binary->op.type) {
      case TokenType::Minus:
        return expect_number(left, "Operands must be numbers.") -
               expect_number(right, "Operands must be numbers.");
      case TokenType::Slash: {
        const auto divisor = expect_number(right, "Operands must be numbers.");
        if (std::fabs(divisor) <= kNumericEpsilon) {
          throw RuntimeError("Division by zero.");
        }
        return expect_number(left, "Operands must be numbers.") / divisor;
      }
      case TokenType::Star:
        return expect_number(left, "Operands must be numbers.") *
               expect_number(right, "Operands must be numbers.");
      case TokenType::Percent: {
        const auto lhs = expect_number(left, "Operands must be numbers.");
        const auto rhs = expect_number(right, "Operands must be numbers.");
        if (std::fabs(rhs) <= kNumericEpsilon) {
          throw RuntimeError("Modulo by zero.");
        }
        return std::fmod(lhs, rhs);
      }
      case TokenType::Plus:
        if (const auto* left_num = std::get_if<double>(&left);
            left_num && std::holds_alternative<double>(right)) {
          return *left_num + std::get<double>(right);
        }
        return stringify(left) + stringify(right);
      case TokenType::Greater:
        return expect_number(left, "Operands must be numbers.") >
               expect_number(right, "Operands must be numbers.");
      case TokenType::GreaterEqual:
        return expect_number(left, "Operands must be numbers.") >=
               expect_number(right, "Operands must be numbers.");
      case TokenType::Less:
        return expect_number(left, "Operands must be numbers.") <
               expect_number(right, "Operands must be numbers.");
      case TokenType::LessEqual:
        return expect_number(left, "Operands must be numbers.") <=
               expect_number(right, "Operands must be numbers.");
      case TokenType::BangEqual:
        return !is_equal(left, right);
      case TokenType::EqualEqual:
        return is_equal(left, right);
      default:
        break;
    }
  }

  if (const auto* call = dynamic_cast<CallExpr*>(expr.get())) {
    auto callee = evaluate(call->callee);
    std::vector<Value> args;
    args.reserve(call->arguments.size());
    for (const auto& argument : call->arguments) {
      args.push_back(evaluate(argument));
    }

    auto* function_ptr = std::get_if<std::shared_ptr<Callable>>(&callee);
    if (function_ptr == nullptr || !(*function_ptr)) {
      throw RuntimeError("Can only call functions.");
    }

    if (static_cast<int>(args.size()) != (*function_ptr)->arity()) {
      throw RuntimeError("Expected " + std::to_string((*function_ptr)->arity()) +
                         " arguments but got " + std::to_string(args.size()) + ".");
    }

    return (*function_ptr)->call(*this, args);
  }

  throw RuntimeError("Unexpected expression.");
}

void Interpreter::execute(const StmtPtr& statement) {
  if (const auto* expr_stmt = dynamic_cast<ExpressionStmt*>(statement.get())) {
    static_cast<void>(evaluate(expr_stmt->expression));
    return;
  }

  if (const auto* print_stmt = dynamic_cast<PrintStmt*>(statement.get())) {
    output_ << stringify(evaluate(print_stmt->expression)) << "\n";
    return;
  }

  if (const auto* var_stmt = dynamic_cast<VarStmt*>(statement.get())) {
    Value value = std::monostate{};
    if (var_stmt->initializer) {
      value = evaluate(var_stmt->initializer);
    }
    environment_->define(var_stmt->name.lexeme, value);
    return;
  }

  if (const auto* block_stmt = dynamic_cast<BlockStmt*>(statement.get())) {
    execute_block(block_stmt->statements, std::make_shared<Environment>(environment_));
    return;
  }

  if (const auto* if_stmt = dynamic_cast<IfStmt*>(statement.get())) {
    if (is_truthy(evaluate(if_stmt->condition))) {
      execute(if_stmt->then_branch);
    } else if (if_stmt->else_branch) {
      execute(if_stmt->else_branch);
    }
    return;
  }

  if (const auto* while_stmt = dynamic_cast<WhileStmt*>(statement.get())) {
    while (is_truthy(evaluate(while_stmt->condition))) {
      execute(while_stmt->body);
    }
    return;
  }

  if (const auto* function_stmt = dynamic_cast<FunctionStmt*>(statement.get())) {
    auto function = std::make_shared<UserFunction>(
        std::make_shared<FunctionStmt>(*function_stmt), environment_);
    environment_->define(function_stmt->name.lexeme, function);
    return;
  }

  if (const auto* return_stmt = dynamic_cast<ReturnStmt*>(statement.get())) {
    Value value = std::monostate{};
    if (return_stmt->value) {
      value = evaluate(return_stmt->value);
    }
    throw ReturnSignal(value);
  }

  throw RuntimeError("Unexpected statement.");
}

}  // namespace pocketpp
