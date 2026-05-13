// pocketpp.cpp – public entry point only; all implementation lives in the
// individual modules (lexer, parser, interpreter, …).

#include "pocketpp/pocketpp.hpp"

#include "interpreter.hpp"
#include "lexer.hpp"
#include "parser.hpp"

namespace pocketpp {

RunResult run_script(const std::string& source) {
  try {
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    Parser parser(std::move(tokens));
    auto statements = parser.parse();

    Interpreter interpreter;
    interpreter.interpret(statements);
    return RunResult{interpreter.output(), std::nullopt};
  } catch (const std::exception& ex) {
    return RunResult{"", ex.what()};
  }
}

}  // namespace pocketpp
