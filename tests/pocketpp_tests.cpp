#include "pocketpp/pocketpp.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("arithmetic and print") {
  const auto result = pocketpp::run_script("print 1 + 2 * 3;\n");
  REQUIRE_FALSE(result.error.has_value());
  REQUIRE(result.output == "7\n");
}

TEST_CASE("variables and assignment") {
  const auto result = pocketpp::run_script("var a = 3; a = a + 4; print a;\n");
  REQUIRE_FALSE(result.error.has_value());
  REQUIRE(result.output == "7\n");
}

TEST_CASE("if else and while") {
  const auto result = pocketpp::run_script(
      "var i = 0;\n"
      "var s = 0;\n"
      "while (i < 4) {\n"
      "  if (i % 2 == 0) { s = s + i; }\n"
      "  i = i + 1;\n"
      "}\n"
      "print s;\n");

  REQUIRE_FALSE(result.error.has_value());
  REQUIRE(result.output == "2\n");
}

TEST_CASE("functions and return") {
  const auto result = pocketpp::run_script(
      "fun add(a, b) {\n"
      "  return a + b;\n"
      "}\n"
      "print add(5, 6);\n");

  REQUIRE_FALSE(result.error.has_value());
  REQUIRE(result.output == "11\n");
}

TEST_CASE("runtime error propagation") {
  const auto result = pocketpp::run_script("print unknown_name;\n");
  REQUIRE(result.error.has_value());
}
