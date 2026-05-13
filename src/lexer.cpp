#include "lexer.hpp"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pocketpp {

Lexer::Lexer(std::string source) : source_(std::move(source)) {}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> tokens;
  while (!is_at_end()) {
    start_ = current_;
    scan_token(tokens);
  }
  tokens.push_back(Token{TokenType::EndOfFile, "", line_});
  return tokens;
}

bool Lexer::is_at_end() const { return current_ >= source_.size(); }

char Lexer::advance() { return source_.at(current_++); }

bool Lexer::match(char expected) {
  if (is_at_end() || source_.at(current_) != expected) {
    return false;
  }
  ++current_;
  return true;
}

char Lexer::peek() const {
  if (is_at_end()) {
    return '\0';
  }
  return source_.at(current_);
}

char Lexer::peek_next() const {
  if (current_ + 1 >= source_.size()) {
    return '\0';
  }
  return source_.at(current_ + 1);
}

void Lexer::add_token(std::vector<Token>& tokens, TokenType type) {
  tokens.push_back(Token{type, source_.substr(start_, current_ - start_), line_});
}

bool Lexer::is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool Lexer::is_alpha_numeric(char c) { return is_alpha(c) || (c >= '0' && c <= '9'); }

void Lexer::identifier(std::vector<Token>& tokens) {
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

void Lexer::number(std::vector<Token>& tokens) {
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

void Lexer::string(std::vector<Token>& tokens) {
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

void Lexer::scan_token(std::vector<Token>& tokens) {
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

}  // namespace pocketpp
