#pragma once

#include "errors.hpp"
#include "token.hpp"

#include <string>
#include <vector>

namespace pocketpp {

class Lexer {
 public:
  explicit Lexer(std::string source);

  std::vector<Token> tokenize();

 private:
  bool is_at_end() const;
  char advance();
  bool match(char expected);
  char peek() const;
  char peek_next() const;
  void add_token(std::vector<Token>& tokens, TokenType type);

  static bool is_alpha(char c);
  static bool is_alpha_numeric(char c);

  void identifier(std::vector<Token>& tokens);
  void number(std::vector<Token>& tokens);
  void string(std::vector<Token>& tokens);
  void scan_token(std::vector<Token>& tokens);

  std::string source_;
  std::size_t start_ = 0;
  std::size_t current_ = 0;
  std::size_t line_ = 1;
};

}  // namespace pocketpp
