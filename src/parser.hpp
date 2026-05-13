#pragma once

#include "ast.hpp"
#include "errors.hpp"
#include "token.hpp"

#include <initializer_list>
#include <string>
#include <vector>

namespace pocketpp {

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens);

  std::vector<StmtPtr> parse();

 private:
  bool is_at_end() const;
  const Token& peek() const;
  const Token& previous() const;
  const Token& advance();
  bool check(TokenType type) const;
  bool match(std::initializer_list<TokenType> types);
  Token consume(TokenType type, const std::string& message);

  StmtPtr declaration();
  StmtPtr function_declaration();
  StmtPtr var_declaration();
  StmtPtr statement();
  StmtPtr for_statement();
  StmtPtr if_statement();
  StmtPtr print_statement();
  StmtPtr return_statement();
  StmtPtr while_statement();
  std::vector<StmtPtr> block();
  StmtPtr expression_statement();

  ExprPtr expression();
  ExprPtr assignment();
  ExprPtr or_expression();
  ExprPtr and_expression();
  ExprPtr equality();
  ExprPtr comparison();
  ExprPtr term();
  ExprPtr factor();
  ExprPtr unary();
  ExprPtr call();
  ExprPtr finish_call(ExprPtr callee);
  ExprPtr primary();

  std::vector<Token> tokens_;
  std::size_t current_ = 0;
};

}  // namespace pocketpp
