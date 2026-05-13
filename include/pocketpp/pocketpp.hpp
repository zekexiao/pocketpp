#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace pocketpp {

struct RunResult {
  std::string output;
  std::optional<std::string> error;
};

RunResult run_script(const std::string& source);

}  // namespace pocketpp
