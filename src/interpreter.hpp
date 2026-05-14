#pragma once
#include "ast.hpp"
#include "environment.hpp"
#include "errors.hpp"
#include "value.hpp"
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>

namespace pocketpp {

// Control-flow signals (thrown as exceptions)
struct ReturnSignal  { Value value; };
struct BreakSignal   {};
struct YieldSignal   { Value value; };

// TCO signal (direct tail call to same function)
struct TailCallSignal {
    std::shared_ptr<FuncData> fn;
    std::vector<Value> args;
};

// Fiber implementation (opaque in header, defined in .cpp)
struct FiberImpl;

class Interpreter {
public:
    explicit Interpreter(std::string base_dir = "");
    void run(const std::vector<StmtPtr>& stmts);
    std::string output() const { return out_.str(); }
    std::string error()  const { return err_; }

    void exec_block(const std::vector<StmtPtr>& stmts,
                    std::shared_ptr<Environment> env);

    Value eval(const ExprPtr& e);
    Value call_value(const Value& callee, std::vector<Value> args);

    std::shared_ptr<Environment> globals_;
    std::shared_ptr<Environment> env_;
    std::ostringstream out_;

    // Flat globals for the bytecode VM
    std::unordered_map<std::string, Value> flat_globals_;

    // Current fiber context (for yield)
    FiberImpl* current_fiber_{nullptr};

private:
    std::string err_;
    std::string base_dir_;

    // Class context for super/self
    std::shared_ptr<ClassData> current_class_;
    Value current_self_;

    // TCO: current function being executed (for direct tail call detection)
    std::shared_ptr<FuncData> tco_fn_;

    // Module cache
    std::unordered_map<std::string, Value> modules_;

    void exec(const StmtPtr& s);

    void register_builtins();
    Value make_native(const std::string& name, int arity,
                      std::function<Value(std::vector<Value>)> fn);

    static bool is_truthy(const Value& v);
    std::string to_str(const Value& v, std::vector<const void*>* seen = nullptr);
    Value apply_op(TT op, Value lhs, Value rhs);
    Value do_binary(const Value& l, const Token& op, const Value& r);
    Value do_index(const Value& obj, const Value& idx, int line);
    void  do_index_assign(Value& obj, const Value& idx, TT op, Value rhs, int line);
    Value do_get(const Value& obj, const std::string& name, int line);
    void  do_set(Value& obj, const std::string& name, TT op, Value rhs, int line);
    Value slice(const Value& obj, double a, double b, int line);

    Value call_method(Value self, const std::string& method, std::vector<Value> args, int line);
    Value get_attr(const Value& obj, const std::string& name, int line);

    Value import_module(const std::string& path, const std::string& from_file="");
    void  exec_module(const std::string& path, ModuleData& mod);
};

} // namespace pocketpp
