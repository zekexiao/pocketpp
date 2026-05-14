#pragma once
#include <optional>
#include <string>

namespace pocketpp {

struct RunResult {
    std::string output;
    std::optional<std::string> error;
};

// Run a pocketlang script.
// base_dir: directory for resolving imports (typically dir of the script file)
RunResult run_script(const std::string& source, const std::string& base_dir = "");

} // namespace pocketpp
