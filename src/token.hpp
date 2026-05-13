#pragma once

#include <cstddef>
#include <string>

namespace pocketpp {

enum class TokenType {
  // Single-character tokens
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

  // One-or-two character tokens
  Bang,
  BangEqual,
  Equal,
  EqualEqual,
  Greater,
  GreaterEqual,
  Less,
  LessEqual,

  // Literals
  Identifier,
  String,
  Number,

  // Keywords
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

// Shared numeric/parser limits
constexpr std::size_t kMaxParameters = 255;
constexpr std::size_t kMaxArguments = 255;
constexpr double kNumericEpsilon = 1e-12;

}  // namespace pocketpp
