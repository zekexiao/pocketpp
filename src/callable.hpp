#pragma once

#include "value.hpp"

#include <string>
#include <vector>

namespace pocketpp {

// Forward declaration – full definition is in interpreter.hpp
class Interpreter;

struct Callable {
  virtual ~Callable() = default;
  virtual int arity() const = 0;
  virtual Value call(Interpreter& interpreter, const std::vector<Value>& args) = 0;
  virtual std::string to_string() const = 0;
};

}  // namespace pocketpp
