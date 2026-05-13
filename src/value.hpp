#pragma once

#include <memory>
#include <string>
#include <variant>

namespace pocketpp {

// Forward declaration – full definition is in callable.hpp
struct Callable;

// The runtime value type used throughout the interpreter.
using Value = std::variant<std::monostate, bool, double, std::string, std::shared_ptr<Callable>>;

}  // namespace pocketpp
