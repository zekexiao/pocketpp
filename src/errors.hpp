#pragma once
#include <stdexcept>
#include <string>

namespace pocketpp {

struct LexError : std::runtime_error {
    explicit LexError(const std::string& m) : std::runtime_error(m) {}
};
struct ParseError : std::runtime_error {
    explicit ParseError(const std::string& m) : std::runtime_error(m) {}
};
struct RuntimeError : std::runtime_error {
    explicit RuntimeError(const std::string& m) : std::runtime_error(m) {}
};
struct AssertError : RuntimeError {
    explicit AssertError(const std::string& m) : RuntimeError(m) {}
};

} // namespace pocketpp
