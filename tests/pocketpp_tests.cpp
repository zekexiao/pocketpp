#include "pocketpp/pocketpp.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

// ── Lang test file runner ─────────────────────────────────────────────────────

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

static void run_lang_test(const std::string& filename) {
    std::string tests_lang_dir = TESTS_LANG_DIR;
    std::string path = tests_lang_dir + "/" + filename;
    std::string src  = read_file(path);
    REQUIRE_FALSE(src.empty());
    std::string base = fs::path(path).parent_path().string();
    auto result = pocketpp::run_script(src, base);
    INFO("Error: " << (result.error ? *result.error : "(none)"));
    INFO("Output:\n" << result.output);
    REQUIRE_FALSE(result.error.has_value());
    REQUIRE(result.output.find("All TESTS PASSED") != std::string::npos);
}

TEST_CASE("functions.pk") { run_lang_test("functions.pk"); }
TEST_CASE("controlflow.pk") { run_lang_test("controlflow.pk"); }
TEST_CASE("closure.pk") { run_lang_test("closure.pk"); }
TEST_CASE("basics.pk") { run_lang_test("basics.pk"); }
TEST_CASE("builtin_fn.pk") { run_lang_test("builtin_fn.pk"); }
TEST_CASE("builtin_ty.pk") { run_lang_test("builtin_ty.pk"); }
TEST_CASE("class.pk") { run_lang_test("class.pk"); }
TEST_CASE("tco.pk") { run_lang_test("tco.pk"); }
TEST_CASE("fibers.pk") { run_lang_test("fibers.pk"); }
TEST_CASE("import.pk") { run_lang_test("import.pk"); }
