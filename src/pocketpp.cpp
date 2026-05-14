// pocketpp.cpp – public entry point
#include "pocketpp/pocketpp.hpp"
#include "interpreter.hpp"
#include "lexer.hpp"
#include "parser.hpp"

namespace pocketpp {

RunResult run_script(const std::string& source, const std::string& base_dir) {
    try {
        Lexer lexer(source);
        auto tokens = lexer.tokenize();
        Parser parser(std::move(tokens));
        auto stmts = parser.parse();
        Interpreter interp(base_dir);
        interp.run(stmts);
        return {interp.output(), std::nullopt};
    } catch (const std::exception& ex) {
        return {"", ex.what()};
    }
}

} // namespace pocketpp
