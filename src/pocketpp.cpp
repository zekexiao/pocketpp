#include "pocketpp/pocketpp.hpp"

#include <cmath>
#include <cstddef>
#include <ctime>
#include <initializer_list>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace pocketpp {
namespace {

enum class TokenType {
  LeftParen,
  RightParen,
  LeftBrace,
  RightBrace,
  Comma,
  Dot,
  Minus,
  Plus,
  Semicolon,
  Slash,
  Star,
  Percent,

  Bang,
  BangEqual,
  Equal,
  EqualEqual,
  Greater,
  GreaterEqual,
  Less,
  LessEqual,

  Identifier,
  String,
  Number,

  And,
  Else,
  False,
  Fun,
  For,
  If,
  Nil,
  Or,
  Print,
  Return,
  True,
  Var,
  While,

  EndOfFile,
};

struct Token {
  TokenType type;
  std::string lexeme;
  std::size_t line;
};

struct LexError : std::runtime_error {
  explicit LexError(const std::string& msg) : std::runtime_error(msg) {}
};

struct ParseError : std::runtime_error {
  explicit ParseError(const std::string& msg) : std::runtime_error(msg) {}
};

struct RuntimeError : std::runtime_error {
  explicit RuntimeError(const std::string& msg) : std::runtime_error(msg) {}
};

constexpr std::size_t kMaxParameters = 255;
constexpr std::size_t kMaxArguments = 255;
constexpr double kNumericEpsilon = 1e-12;

class Lexer {
 public:
  explicit Lexer(std::string source) : source_(std::move(source)) {}

  std::vector<Token> tokenize() {
    std::vector<Token> tokens;
    while (!is_at_end()) {
      start_ = current_;
      scan_token(tokens);
    }
    tokens.push_back(Token{TokenType::EndOfFile, "", line_});
    return tokens;
  }

 private:
  bool is_at_end() const { return current_ >= source_.size(); }

  char advance() { return source_.at(current_++); }

  bool match(char expected) {
    if (is_at_end() || source_.at(current_) != expected) {
      return false;
    }
    ++current_;
    return true;
  }

  char peek() const {
    if (is_at_end()) {
      return '\0';
    }
    return source_.at(current_);
  }

  char peek_next() const {
    if (current_ + 1 >= source_.size()) {
      return '\0';
    }
    return source_.at(current_ + 1);
  }

  void add_token(std::vector<Token>& tokens, TokenType type) {
    tokens.push_back(Token{type, source_.substr(start_, current_ - start_), line_});
  }

  static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  }

  static bool is_alpha_numeric(char c) { return is_alpha(c) || (c >= '0' && c <= '9'); }

  void identifier(std::vector<Token>& tokens) {
    while (is_alpha_numeric(peek())) {
      advance();
    }

    static const std::unordered_map<std::string, TokenType> keywords{
        {"and", TokenType::And},       {"else", TokenType::Else},   {"false", TokenType::False},
        {"for", TokenType::For},       {"fun", TokenType::Fun},     {"if", TokenType::If},
        {"nil", TokenType::Nil},       {"or", TokenType::Or},       {"print", TokenType::Print},
        {"return", TokenType::Return}, {"true", TokenType::True},   {"var", TokenType::Var},
        {"while", TokenType::While},
    };

    const auto text = source_.substr(start_, current_ - start_);
    const auto it = keywords.find(text);
    add_token(tokens, it == keywords.end() ? TokenType::Identifier : it->second);
  }

  void number(std::vector<Token>& tokens) {
    while (peek() >= '0' && peek() <= '9') {
      advance();
    }

    if (peek() == '.' && peek_next() >= '0' && peek_next() <= '9') {
      advance();
      while (peek() >= '0' && peek() <= '9') {
        advance();
      }
    }

    add_token(tokens, TokenType::Number);
  }

  void string(std::vector<Token>& tokens) {
    while (peek() != '"' && !is_at_end()) {
      if (peek() == '\n') {
        ++line_;
      }
      advance();
    }

    if (is_at_end()) {
      throw LexError("[line " + std::to_string(line_) + "] Unterminated string.");
    }

    advance();
    add_token(tokens, TokenType::String);
  }

  void scan_token(std::vector<Token>& tokens) {
    switch (const char c = advance()) {
      case '(':
        add_token(tokens, TokenType::LeftParen);
        break;
      case ')':
        add_token(tokens, TokenType::RightParen);
        break;
      case '{':
        add_token(tokens, TokenType::LeftBrace);
        break;
      case '}':
        add_token(tokens, TokenType::RightBrace);
        break;
      case ',':
        add_token(tokens, TokenType::Comma);
        break;
      case '.':
        add_token(tokens, TokenType::Dot);
        break;
      case '-':
        add_token(tokens, TokenType::Minus);
        break;
      case '+':
        add_token(tokens, TokenType::Plus);
        break;
      case ';':
        add_token(tokens, TokenType::Semicolon);
        break;
      case '*':
        add_token(tokens, TokenType::Star);
        break;
      case '%':
        add_token(tokens, TokenType::Percent);
        break;
      case '!':
        add_token(tokens, match('=') ? TokenType::BangEqual : TokenType::Bang);
        break;
      case '=':
        add_token(tokens, match('=') ? TokenType::EqualEqual : TokenType::Equal);
        break;
      case '<':
        add_token(tokens, match('=') ? TokenType::LessEqual : TokenType::Less);
        break;
      case '>':
        add_token(tokens, match('=') ? TokenType::GreaterEqual : TokenType::Greater);
        break;
      case '/':
        if (match('/')) {
          while (peek() != '\n' && !is_at_end()) {
            advance();
          }
        } else {
          add_token(tokens, TokenType::Slash);
        }
        break;
      case ' ':
      case '\r':
      case '\t':
        break;
      case '\n':
        ++line_;
        break;
      case '"':
        string(tokens);
        break;
      default:
        if (c >= '0' && c <= '9') {
          number(tokens);
        } else if (is_alpha(c)) {
          identifier(tokens);
        } else {
          throw LexError("[line " + std::to_string(line_) + "] Unexpected character.");
        }
        break;
    }
  }

  std::string source_;
  std::size_t start_ = 0;
  std::size_t current_ = 0;
  std::size_t line_ = 1;
};

struct Callable;
using Value = std::variant<std::monostate, bool, double, std::string, std::shared_ptr<Callable>>;

struct Expr;
struct Stmt;
using ExprPtr = std::shared_ptr<Expr>;
using StmtPtr = std::shared_ptr<Stmt>;

struct Expr {
  virtual ~Expr() = default;
};

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

struct Stmt {
  virtual ~Stmt() = default;
};

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
      : condition(std::move(condition)), then_branch(std::move(then_branch)), else_branch(std::move(else_branch)) {}
  ExprPtr condition;
  StmtPtr then_branch;
  StmtPtr else_branch;
};

struct WhileStmt final : Stmt {
  WhileStmt(ExprPtr condition, StmtPtr body) : condition(std::move(condition)), body(std::move(body)) {}
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
  ReturnStmt(Token keyword, ExprPtr value) : keyword(std::move(keyword)), value(std::move(value)) {}
  Token keyword;
  ExprPtr value;
};

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  std::vector<StmtPtr> parse() {
    std::vector<StmtPtr> statements;
    while (!is_at_end()) {
      statements.push_back(declaration());
    }
    return statements;
  }

 private:
  bool is_at_end() const { return peek().type == TokenType::EndOfFile; }

  const Token& peek() const { return tokens_.at(current_); }

  const Token& previous() const { return tokens_.at(current_ - 1); }

  const Token& advance() {
    if (!is_at_end()) {
      ++current_;
    }
    return previous();
  }

  bool check(TokenType type) const {
    if (is_at_end()) {
      return false;
    }
    return peek().type == type;
  }

  bool match(std::initializer_list<TokenType> types) {
    for (const auto type : types) {
      if (check(type)) {
        advance();
        return true;
      }
    }
    return false;
  }

  Token consume(TokenType type, const std::string& message) {
    if (check(type)) {
      return advance();
    }
    throw ParseError("[line " + std::to_string(peek().line) + "] Error at '" + peek().lexeme + "': " + message);
  }

  StmtPtr declaration() {
    if (match({TokenType::Fun})) {
      return function_declaration();
    }
    if (match({TokenType::Var})) {
      return var_declaration();
    }
    return statement();
  }

  StmtPtr function_declaration() {
    Token name = consume(TokenType::Identifier, "Expect function name.");
    consume(TokenType::LeftParen, "Expect '(' after function name.");
    std::vector<Token> parameters;
    if (!check(TokenType::RightParen)) {
      do {
        if (parameters.size() >= kMaxParameters) {
          throw ParseError("[line " + std::to_string(peek().line) + "] Can't have more than 255 parameters.");
        }
        parameters.push_back(consume(TokenType::Identifier, "Expect parameter name."));
      } while (match({TokenType::Comma}));
    }
    consume(TokenType::RightParen, "Expect ')' after parameters.");
    consume(TokenType::LeftBrace, "Expect '{' before function body.");
    return std::make_shared<FunctionStmt>(name, parameters, block());
  }

  StmtPtr var_declaration() {
    Token name = consume(TokenType::Identifier, "Expect variable name.");

    ExprPtr initializer;
    if (match({TokenType::Equal})) {
      initializer = expression();
    }

    consume(TokenType::Semicolon, "Expect ';' after variable declaration.");
    return std::make_shared<VarStmt>(name, initializer);
  }

  StmtPtr statement() {
    if (match({TokenType::For})) {
      return for_statement();
    }
    if (match({TokenType::If})) {
      return if_statement();
    }
    if (match({TokenType::Print})) {
      return print_statement();
    }
    if (match({TokenType::Return})) {
      return return_statement();
    }
    if (match({TokenType::While})) {
      return while_statement();
    }
    if (match({TokenType::LeftBrace})) {
      return std::make_shared<BlockStmt>(block());
    }

    return expression_statement();
  }

  StmtPtr for_statement() {
    consume(TokenType::LeftParen, "Expect '(' after 'for'.");

    StmtPtr initializer;
    if (match({TokenType::Semicolon})) {
      initializer = nullptr;
    } else if (match({TokenType::Var})) {
      initializer = var_declaration();
    } else {
      initializer = expression_statement();
    }

    ExprPtr condition;
    if (!check(TokenType::Semicolon)) {
      condition = expression();
    }
    consume(TokenType::Semicolon, "Expect ';' after loop condition.");

    ExprPtr increment;
    if (!check(TokenType::RightParen)) {
      increment = expression();
    }
    consume(TokenType::RightParen, "Expect ')' after for clauses.");

    auto body = statement();

    if (increment) {
      std::vector<StmtPtr> statements;
      statements.push_back(body);
      statements.push_back(std::make_shared<ExpressionStmt>(increment));
      body = std::make_shared<BlockStmt>(std::move(statements));
    }

    if (!condition) {
      condition = std::make_shared<LiteralExpr>(true);
    }
    body = std::make_shared<WhileStmt>(condition, body);

    if (initializer) {
      std::vector<StmtPtr> statements;
      statements.push_back(initializer);
      statements.push_back(body);
      body = std::make_shared<BlockStmt>(std::move(statements));
    }

    return body;
  }

  StmtPtr if_statement() {
    consume(TokenType::LeftParen, "Expect '(' after 'if'.");
    auto condition = expression();
    consume(TokenType::RightParen, "Expect ')' after if condition.");

    auto then_branch = statement();
    StmtPtr else_branch;
    if (match({TokenType::Else})) {
      else_branch = statement();
    }

    return std::make_shared<IfStmt>(condition, then_branch, else_branch);
  }

  StmtPtr print_statement() {
    auto value = expression();
    consume(TokenType::Semicolon, "Expect ';' after value.");
    return std::make_shared<PrintStmt>(value);
  }

  StmtPtr return_statement() {
    Token keyword = previous();
    ExprPtr value;
    if (!check(TokenType::Semicolon)) {
      value = expression();
    }

    consume(TokenType::Semicolon, "Expect ';' after return value.");
    return std::make_shared<ReturnStmt>(keyword, value);
  }

  StmtPtr while_statement() {
    consume(TokenType::LeftParen, "Expect '(' after 'while'.");
    auto condition = expression();
    consume(TokenType::RightParen, "Expect ')' after condition.");
    auto body = statement();

    return std::make_shared<WhileStmt>(condition, body);
  }

  std::vector<StmtPtr> block() {
    std::vector<StmtPtr> statements;

    while (!check(TokenType::RightBrace) && !is_at_end()) {
      statements.push_back(declaration());
    }

    consume(TokenType::RightBrace, "Expect '}' after block.");
    return statements;
  }

  StmtPtr expression_statement() {
    auto expr = expression();
    consume(TokenType::Semicolon, "Expect ';' after expression.");
    return std::make_shared<ExpressionStmt>(expr);
  }

  ExprPtr expression() { return assignment(); }

  ExprPtr assignment() {
    auto expr = or_expression();

    if (match({TokenType::Equal})) {
      Token equals = previous();
      auto value = assignment();

      auto* variable = dynamic_cast<VariableExpr*>(expr.get());
      if (variable != nullptr) {
        return std::make_shared<AssignExpr>(variable->name, value);
      }

      throw ParseError("[line " + std::to_string(equals.line) + "] Invalid assignment target.");
    }

    return expr;
  }

  ExprPtr or_expression() {
    auto expr = and_expression();

    while (match({TokenType::Or})) {
      Token op = previous();
      auto right = and_expression();
      expr = std::make_shared<LogicalExpr>(expr, op, right);
    }

    return expr;
  }

  ExprPtr and_expression() {
    auto expr = equality();

    while (match({TokenType::And})) {
      Token op = previous();
      auto right = equality();
      expr = std::make_shared<LogicalExpr>(expr, op, right);
    }

    return expr;
  }

  ExprPtr equality() {
    auto expr = comparison();

    while (match({TokenType::BangEqual, TokenType::EqualEqual})) {
      Token op = previous();
      auto right = comparison();
      expr = std::make_shared<BinaryExpr>(expr, op, right);
    }

    return expr;
  }

  ExprPtr comparison() {
    auto expr = term();

    while (match({TokenType::Greater, TokenType::GreaterEqual, TokenType::Less, TokenType::LessEqual})) {
      Token op = previous();
      auto right = term();
      expr = std::make_shared<BinaryExpr>(expr, op, right);
    }

    return expr;
  }

  ExprPtr term() {
    auto expr = factor();

    while (match({TokenType::Minus, TokenType::Plus})) {
      Token op = previous();
      auto right = factor();
      expr = std::make_shared<BinaryExpr>(expr, op, right);
    }

    return expr;
  }

  ExprPtr factor() {
    auto expr = unary();

    while (match({TokenType::Slash, TokenType::Star, TokenType::Percent})) {
      Token op = previous();
      auto right = unary();
      expr = std::make_shared<BinaryExpr>(expr, op, right);
    }

    return expr;
  }

  ExprPtr unary() {
    if (match({TokenType::Bang, TokenType::Minus})) {
      Token op = previous();
      auto right = unary();
      return std::make_shared<UnaryExpr>(op, right);
    }

    return call();
  }

  ExprPtr call() {
    auto expr = primary();

    while (true) {
      if (match({TokenType::LeftParen})) {
        expr = finish_call(expr);
      } else {
        break;
      }
    }

    return expr;
  }

  ExprPtr finish_call(ExprPtr callee) {
    std::vector<ExprPtr> arguments;
    if (!check(TokenType::RightParen)) {
      do {
        if (arguments.size() >= kMaxArguments) {
          throw ParseError("[line " + std::to_string(peek().line) + "] Can't have more than 255 arguments.");
        }
        arguments.push_back(expression());
      } while (match({TokenType::Comma}));
    }

    Token paren = consume(TokenType::RightParen, "Expect ')' after arguments.");

    return std::make_shared<CallExpr>(callee, paren, arguments);
  }

  ExprPtr primary() {
    if (match({TokenType::False})) {
      return std::make_shared<LiteralExpr>(false);
    }
    if (match({TokenType::True})) {
      return std::make_shared<LiteralExpr>(true);
    }
    if (match({TokenType::Nil})) {
      return std::make_shared<LiteralExpr>(std::monostate{});
    }

    if (match({TokenType::Number})) {
      return std::make_shared<LiteralExpr>(std::stod(previous().lexeme));
    }

    if (match({TokenType::String})) {
      const std::string quoted = previous().lexeme;
      return std::make_shared<LiteralExpr>(quoted.substr(1, quoted.size() - 2));
    }

    if (match({TokenType::Identifier})) {
      return std::make_shared<VariableExpr>(previous());
    }

    if (match({TokenType::LeftParen})) {
      auto expr = expression();
      consume(TokenType::RightParen, "Expect ')' after expression.");
      return std::make_shared<GroupingExpr>(expr);
    }

    throw ParseError("[line " + std::to_string(peek().line) + "] Expect expression.");
  }

  std::vector<Token> tokens_;
  std::size_t current_ = 0;
};

class Interpreter;

struct Callable {
  virtual ~Callable() = default;
  virtual int arity() const = 0;
  virtual Value call(Interpreter& interpreter, const std::vector<Value>& args) = 0;
  virtual std::string to_string() const = 0;
};

class Environment : public std::enable_shared_from_this<Environment> {
 public:
  explicit Environment(std::shared_ptr<Environment> enclosing = nullptr) : enclosing_(std::move(enclosing)) {}

  void define(const std::string& name, Value value) { values_[name] = std::move(value); }

  Value get(const Token& name) const {
    const auto it = values_.find(name.lexeme);
    if (it != values_.end()) {
      return it->second;
    }

    if (enclosing_) {
      return enclosing_->get(name);
    }

    throw RuntimeError("Undefined variable '" + name.lexeme + "'.");
  }

  void assign(const Token& name, Value value) {
    const auto it = values_.find(name.lexeme);
    if (it != values_.end()) {
      it->second = std::move(value);
      return;
    }

    if (enclosing_) {
      enclosing_->assign(name, std::move(value));
      return;
    }

    throw RuntimeError("Undefined variable '" + name.lexeme + "'.");
  }

 private:
  mutable std::unordered_map<std::string, Value> values_;
  std::shared_ptr<Environment> enclosing_;
};

class ReturnSignal : public std::runtime_error {
 public:
  explicit ReturnSignal(Value value) : std::runtime_error("return"), value(std::move(value)) {}

  Value value;
};

class UserFunction final : public Callable {
 public:
  UserFunction(std::shared_ptr<FunctionStmt> declaration, std::shared_ptr<Environment> closure)
      : declaration_(std::move(declaration)), closure_(std::move(closure)) {}

  int arity() const override { return static_cast<int>(declaration_->params.size()); }

  Value call(Interpreter& interpreter, const std::vector<Value>& args) override;

  std::string to_string() const override { return "<fn " + declaration_->name.lexeme + ">"; }

 private:
  std::shared_ptr<FunctionStmt> declaration_;
  std::shared_ptr<Environment> closure_;
};

class ClockNative final : public Callable {
 public:
  int arity() const override { return 0; }

  Value call(Interpreter&, const std::vector<Value>&) override {
    return static_cast<double>(std::time(nullptr));
  }

  std::string to_string() const override { return "<native fn>"; }
};

class Interpreter {
 public:
  Interpreter() : globals_(std::make_shared<Environment>()), environment_(globals_) {
    globals_->define("clock", std::make_shared<ClockNative>());
  }

  void interpret(const std::vector<StmtPtr>& statements) {
    for (const auto& statement : statements) {
      execute(statement);
    }
  }

  std::string output() const { return output_.str(); }

  void execute_block(const std::vector<StmtPtr>& statements, const std::shared_ptr<Environment>& environment) {
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

 private:
  static bool is_truthy(const Value& value) {
    if (std::holds_alternative<std::monostate>(value)) {
      return false;
    }
    if (const auto* b = std::get_if<bool>(&value)) {
      return *b;
    }
    return true;
  }

  static bool is_equal(const Value& a, const Value& b) {
    return a == b;
  }

  static std::string stringify(const Value& value) {
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

  static double expect_number(const Value& value, const std::string& message) {
    if (const auto* number = std::get_if<double>(&value)) {
      return *number;
    }
    throw RuntimeError(message);
  }

  Value evaluate(const ExprPtr& expr) {
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
          if (const auto* left_num = std::get_if<double>(&left); left_num && std::holds_alternative<double>(right)) {
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
        throw RuntimeError("Expected " + std::to_string((*function_ptr)->arity()) + " arguments but got " +
                           std::to_string(args.size()) + ".");
      }

      return (*function_ptr)->call(*this, args);
    }

    throw RuntimeError("Unexpected expression.");
  }

  void execute(const StmtPtr& statement) {
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
      auto function = std::make_shared<UserFunction>(std::make_shared<FunctionStmt>(*function_stmt), environment_);
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

  std::shared_ptr<Environment> globals_;
  std::shared_ptr<Environment> environment_;
  std::ostringstream output_;

  friend class UserFunction;
};

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

}  // namespace

RunResult run_script(const std::string& source) {
  try {
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    Parser parser(std::move(tokens));
    auto statements = parser.parse();

    Interpreter interpreter;
    interpreter.interpret(statements);
    return RunResult{interpreter.output(), std::nullopt};
  } catch (const std::exception& ex) {
    return RunResult{"", ex.what()};
  }
}

}  // namespace pocketpp
