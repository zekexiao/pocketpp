#pragma once

#include "errors.hpp"
#include "token.hpp"
#include "value.hpp"

#include <memory>
#include <string>
#include <unordered_map>

namespace pocketpp {

class Environment : public std::enable_shared_from_this<Environment> {
 public:
  explicit Environment(std::shared_ptr<Environment> enclosing = nullptr)
      : enclosing_(std::move(enclosing)) {}

  void define(const std::string& name, Value value);
  Value get(const Token& name) const;
  void assign(const Token& name, Value value);

 private:
  mutable std::unordered_map<std::string, Value> values_;
  std::shared_ptr<Environment> enclosing_;
};

}  // namespace pocketpp
