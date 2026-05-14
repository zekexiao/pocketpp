#include "pocketpp/pocketpp.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    std::string source;
    std::string base_dir;

    if (argc > 1) {
        std::ifstream file(argv[1]);
        if (!file) { std::cerr << "Failed to open file: " << argv[1] << '\n'; return 1; }
        std::ostringstream ss; ss << file.rdbuf(); source = ss.str();
        base_dir = fs::path(argv[1]).parent_path().string();
        if (base_dir.empty()) base_dir = ".";
    } else {
        std::ostringstream ss; ss << std::cin.rdbuf(); source = ss.str();
        base_dir = ".";
    }

    auto result = pocketpp::run_script(source, base_dir);
    if (result.error) { std::cerr << *result.error << '\n'; return 1; }
    std::cout << result.output;
    return 0;
}
