#include "pocketpp/pocketpp.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

namespace {

std::string read_all(std::istream& input) {
  std::ostringstream ss;
  ss << input.rdbuf();
  return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
  std::string source;

  if (argc > 1) {
    std::ifstream file(argv[1]);
    if (!file) {
      std::cerr << "Failed to open file: " << argv[1] << '\n';
      return 1;
    }
    source = read_all(file);
  } else {
    source = read_all(std::cin);
  }

  const auto result = pocketpp::run_script(source);
  if (result.error) {
    std::cerr << *result.error << '\n';
    return 1;
  }

  std::cout << result.output;
  return 0;
}
