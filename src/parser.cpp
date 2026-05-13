#include "parser.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pocketpp {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

std::vector<StmtPtr> Parser::parse() {
  std::vector<StmtPtr> statements;
  while (!is_at_end()) {
    statements.push_back(declaration());
  }
  return statements;
}

bool Parser::is_at_end() const { return peek().type == TokenType::EndOfFile; }

const Token& Parser::peek() const { return tokens_.at(current_); }

const Token& Parser::previous() const { return tokens_.at(current_ - 1); }

const Token& Parser::advance() {
  if (!is_at_end()) {
    ++current_;
  }
  return previous();
}

bool Parser::check(TokenType type) const {
  if (is_at_end()) {
    return false;
  }
  return peek().type == type;
}

bool Parser::match(std::initializer_list<TokenType> types) {
  for (const auto type : types) {
    if (check(type)) {
      advance();
      return true;
    }
  }
  return false;
}

Token Parser::consume(TokenType type, const std::string& message) {
  if (check(type)) {
    return advance();
  }
  throw ParseError("[line " + std::to_string(peek().line) + "] Error at '" + peek().lexeme + "': " + message);
}

StmtPtr Parser::declaration() {
  if (match({TokenType::Fun})) {
    return function_declaration();
  }
  if (match({TokenType::Var})) {
    return var_declaration();
  }
  return statement();
}

StmtPtr Parser::function_declaration() {
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

StmtPtr Parser::var_declaration() {
  Token name = consume(TokenType::Identifier, "Expect variable name.");

  ExprPtr initializer;
  if (match({TokenType::Equal})) {
    initializer = expression();
  }

  consume(TokenType::Semicolon, "Expect ';' after variable declaration.");
  return std::make_shared<VarStmt>(name, initializer);
}

StmtPtr Parser::statement() {
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

StmtPtr Parser::for_statement() {
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
    std::vector<StmtPtr> stmts;
    stmts.push_back(body);
    stmts.push_back(std::make_shared<ExpressionStmt>(increment));
    body = std::make_shared<BlockStmt>(std::move(stmts));
  }

  if (!condition) {
    condition = std::make_shared<LiteralExpr>(true);
  }
  body = std::make_shared<WhileStmt>(condition, body);

  if (initializer) {
    std::vector<StmtPtr> stmts;
    stmts.push_back(initializer);
    stmts.push_back(body);
    body = std::make_shared<BlockStmt>(std::move(stmts));
  }

  return body;
}

StmtPtr Parser::if_statement() {
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

StmtPtr Parser::print_statement() {
  auto value = expression();
  consume(TokenType::Semicolon, "Expect ';' after value.");
  return std::make_shared<PrintStmt>(value);
}

StmtPtr Parser::return_statement() {
  Token keyword = previous();
  ExprPtr value;
  if (!check(TokenType::Semicolon)) {
    value = expression();
  }

  consume(TokenType::Semicolon, "Expect ';' after return value.");
  return std::make_shared<ReturnStmt>(keyword, value);
}

StmtPtr Parser::while_statement() {
  consume(TokenType::LeftParen, "Expect '(' after 'while'.");
  auto condition = expression();
  consume(TokenType::RightParen, "Expect ')' after condition.");
  auto body = statement();

  return std::make_shared<WhileStmt>(condition, body);
}

std::vector<StmtPtr> Parser::block() {
  std::vector<StmtPtr> statements;

  while (!check(TokenType::RightBrace) && !is_at_end()) {
    statements.push_back(declaration());
  }

  consume(TokenType::RightBrace, "Expect '}' after block.");
  return statements;
}

StmtPtr Parser::expression_statement() {
  auto expr = expression();
  consume(TokenType::Semicolon, "Expect ';' after expression.");
  return std::make_shared<ExpressionStmt>(expr);
}

ExprPtr Parser::expression() { return assignment(); }

ExprPtr Parser::assignment() {
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

ExprPtr Parser::or_expression() {
  auto expr = and_expression();

  while (match({TokenType::Or})) {
    Token op = previous();
    auto right = and_expression();
    expr = std::make_shared<LogicalExpr>(expr, op, right);
  }

  return expr;
}

ExprPtr Parser::and_expression() {
  auto expr = equality();

  while (match({TokenType::And})) {
    Token op = previous();
    auto right = equality();
    expr = std::make_shared<LogicalExpr>(expr, op, right);
  }

  return expr;
}

ExprPtr Parser::equality() {
  auto expr = comparison();

  while (match({TokenType::BangEqual, TokenType::EqualEqual})) {
    Token op = previous();
    auto right = comparison();
    expr = std::make_shared<BinaryExpr>(expr, op, right);
  }

  return expr;
}

ExprPtr Parser::comparison() {
  auto expr = term();

  while (match({TokenType::Greater, TokenType::GreaterEqual, TokenType::Less, TokenType::LessEqual})) {
    Token op = previous();
    auto right = term();
    expr = std::make_shared<BinaryExpr>(expr, op, right);
  }

  return expr;
}

ExprPtr Parser::term() {
  auto expr = factor();

  while (match({TokenType::Minus, TokenType::Plus})) {
    Token op = previous();
    auto right = factor();
    expr = std::make_shared<BinaryExpr>(expr, op, right);
  }

  return expr;
}

ExprPtr Parser::factor() {
  auto expr = unary();

  while (match({TokenType::Slash, TokenType::Star, TokenType::Percent})) {
    Token op = previous();
    auto right = unary();
    expr = std::make_shared<BinaryExpr>(expr, op, right);
  }

  return expr;
}

ExprPtr Parser::unary() {
  if (match({TokenType::Bang, TokenType::Minus})) {
    Token op = previous();
    auto right = unary();
    return std::make_shared<UnaryExpr>(op, right);
  }

  return call();
}

ExprPtr Parser::call() {
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

ExprPtr Parser::finish_call(ExprPtr callee) {
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

ExprPtr Parser::primary() {
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

}  // namespace pocketpp
