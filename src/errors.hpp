#pragma once

#include <stdexcept>
#include <string>

namespace pocketpp {

struct LexError : std::runtime_error {
  explicit LexError(const std::string& msg) : std::runtime_error(msg) {}
};

struct ParseError : std::runtime_error {
  explicit ParseError(const std::string& msg) : std::runtime_error(msg) {}
};

struct RuntimeError : std::runtime_error {
  explicit RuntimeError(const std::string& msg) : std::runtime_error(msg) {}
};

}  // namespace pocketpp
