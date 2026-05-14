#include "interpreter.hpp"
#include "compiler.hpp"
#include "vm.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <sstream>
#include <set>
#include <stdexcept>

namespace pocketpp {

namespace fs = std::filesystem;

// ── Fiber implementation ──────────────────────────────────────────────────────
struct FiberImpl {
    std::thread thread;
    std::mutex mu;
    std::condition_variable caller_cv;
    std::condition_variable fiber_cv;

    bool caller_ready{false};
    bool fiber_ready{false};
    bool terminated{false};

    FiberState state{FiberState::CREATED};

    Value send_val;    // main → fiber (resume value)
    Value recv_val;    // fiber → main (yield value)
    std::exception_ptr exc_ptr;

    // Interpreter state saved/restored on each context switch
    std::shared_ptr<Environment> caller_env;
    std::shared_ptr<ClassData>   caller_class;
    Value                        caller_self;

    std::shared_ptr<Environment> fiber_env;
    std::shared_ptr<ClassData>   fiber_class;
    Value                        fiber_self;

    ~FiberImpl() {
        if (thread.joinable()) {
            {
                std::lock_guard<std::mutex> lk(mu);
                terminated = true;
                fiber_ready = true;
            }
            fiber_cv.notify_one();
            thread.join();
        }
    }
};

// ── Helpers ──────────────────────────────────────────────────────────────────

static double require_num(const Value& v, const std::string& ctx) {
    if (!v.is_num()) throw RuntimeError("Expected number in " + ctx + " (got " +
        (v.is_str() ? "'"+v.s+"'" : "non-number") + ")");
    return v.n;
}
static long long to_int(double d) { return (long long)d; }

bool Interpreter::is_truthy(const Value& v) {
    if (v.is_null()) return false;
    if (v.is_bool()) return v.b;
    return true;
}

std::string Interpreter::to_str(const Value& v, std::vector<const void*>* seen) {
    switch (v.type) {
    case Value::Type::Null:    return "null";
    case Value::Type::Bool:    return v.b ? "true" : "false";
    case Value::Type::Number: {
        double d = v.n;
        long long i = (long long)d;
        if ((double)i == d && !std::isinf(d)) return std::to_string(i);
        // Try to produce clean output
        std::ostringstream oss;
        // Detect scientific notation need
        if (std::abs(d) >= 1e15 || (std::abs(d) < 1e-4 && d != 0)) {
            oss << d;
        } else {
            oss << d;
        }
        return oss.str();
    }
    case Value::Type::String:  return v.s;
    case Value::Type::Range:   {
        auto fmt=[](double d){ long long i=(long long)d; return (double)i==d?std::to_string(i):[&]{std::ostringstream o;o<<d;return o.str();}(); };
        return fmt(v.range.first)+".."+fmt(v.range.second);
    }
    case Value::Type::Function: return "<fn "+v.fn->name+">";
    case Value::Type::BoundMethod: return "<bound "+v.bound->fn->name+">";
    case Value::Type::Class:   return "<class "+v.cls->name+">";
    case Value::Type::Module:  return "<module "+v.mod->name+">";
    case Value::Type::Fiber:   {
        const char* states[] = {"created","running","suspended","done"};
        auto st = (int)v.fiber->state;
        return std::string("<fiber ") + states[std::min(st,3)] + ">";
    }
    case Value::Type::Instance: {
        auto& inst = *v.inst;
        auto* method = inst.klass->find_method("_str");
        if (!method) method = inst.klass->find_method("_repr");
        if (method && method->is_fn()) {
            auto saved_self = current_self_; auto saved_class = current_class_;
            current_self_ = v; current_class_ = inst.klass;
            Value result;
            try { result = call_value(*method, {}); }
            catch (...) {}
            current_self_ = saved_self; current_class_ = saved_class;
            if (!result.is_null()) return to_str(result);
        }
        return "<"+inst.klass->name+" instance>";
    }
    case Value::Type::List: {
        std::vector<const void*> local_seen_storage;
        auto* local_seen = seen ? seen : &local_seen_storage;
        const void* ptr = v.list.get();
        for (auto p : *local_seen) if (p == ptr) return "[...]";
        local_seen->push_back(ptr);
        std::string r = "[";
        for (size_t i = 0; i < v.list->items.size(); ++i) {
            if (i > 0) r += ", ";
            r += to_str(v.list->items[i], local_seen);
        }
        local_seen->pop_back();
        return r + "]";
    }
    case Value::Type::Map: {
        std::vector<const void*> local_seen_storage;
        auto* local_seen = seen ? seen : &local_seen_storage;
        const void* ptr = v.map.get();
        for (auto p : *local_seen) if (p == ptr) return "{...}";
        local_seen->push_back(ptr);
        std::string r = "{";
        for (size_t i = 0; i < v.map->pairs.size(); ++i) {
            if (i > 0) r += ", ";
            auto& [k, val] = v.map->pairs[i];
            r += "\""+to_str(k)+"\":"+to_str(val, local_seen);
        }
        local_seen->pop_back();
        return r + "}";
    }
    }
    return "?";
}

Value Interpreter::make_native(const std::string& nm, int ar,
                                std::function<Value(std::vector<Value>)> fn) {
    auto f = std::make_shared<FuncData>();
    f->name = nm; f->arity_val = ar; f->call = std::move(fn);
    return Value::make_fn(f);
}

// ── Built-ins ─────────────────────────────────────────────────────────────────

void Interpreter::register_builtins() {
    auto& g = *globals_;

    g.define("print", make_native("print", -1, [this](std::vector<Value> args) {
        std::string sep = "";
        for (auto& a : args) { out_ << sep << to_str(a); sep = " "; }
        out_ << "\n";
        return Value::make_null();
    }));

    g.define("assert", make_native("assert", -1, [this](std::vector<Value> args) {
        if (args.empty()) throw RuntimeError("assert requires at least one argument");
        if (!is_truthy(args[0])) {
            std::string msg = args.size() > 1 ? to_str(args[1]) : "Assertion failed.";
            throw AssertError(msg);
        }
        return Value::make_null();
    }));

    g.define("str", make_native("str", 1, [this](std::vector<Value> args) {
        return Value::make_str(to_str(args[0]));
    }));

    g.define("type", make_native("type", 1, [](std::vector<Value> args) {
        static const char* names[] = {"Null","Bool","Number","String","List","Map","Range",
                                       "Function","BoundMethod","Class","Instance","Fiber","Module"};
        return Value::make_str(names[(int)args[0].type]);
    }));

    g.define("hex", make_native("hex", 1, [](std::vector<Value> args) {
        long long n = (long long)args[0].n;
        bool neg = n < 0;
        unsigned long long un = neg ? (unsigned long long)(-n) : (unsigned long long)n;
        char buf[64];
        snprintf(buf, sizeof(buf), "%s0x%llx", neg ? "-" : "", un);
        return Value::make_str(buf);
    }));

    g.define("Number", make_native("Number", 1, [](std::vector<Value> args) {
        if (args[0].is_num()) return args[0];
        if (args[0].is_str()) {
            std::string s = args[0].s;
            bool neg = false;
            if (!s.empty() && s[0]=='-') { neg=true; s=s.substr(1); }
            // Remove leading/trailing whitespace
            while (!s.empty() && isspace(s[0])) s=s.substr(1);
            double v = 0;
            bool ok = false;
            try {
                if (s.size()>2 && s[0]=='0' && (s[1]=='b'||s[1]=='B')) {
                    v = 0; for (char c : s.substr(2)) { v*=2; v += (c-'0'); } ok=true;
                } else if (s.size()>2 && s[0]=='0' && (s[1]=='x'||s[1]=='X')) {
                    v = (double)std::stoull(s, nullptr, 16); ok=true;
                } else {
                    size_t idx; v = std::stod(s, &idx); ok=(idx>0);
                }
            } catch(...) {}
            if (ok) return Value::make_num(neg ? -v : v);
        }
        return Value::make_null();
    }));

    g.define("clock", make_native("clock", 0, [](std::vector<Value>) {
        return Value::make_num((double)std::clock() / CLOCKS_PER_SEC);
    }));

    g.define("dir", make_native("dir", 1, [this](std::vector<Value> args) {
        auto list = std::make_shared<ListData>();
        auto add = [&](const std::string& s){ list->items.push_back(Value::make_str(s)); };
        auto& v = args[0];
        if (v.is_inst()) {
            std::set<std::string> seen;
            for (auto& [k,_] : v.inst->attrs) { if (!seen.count(k)){add(k);seen.insert(k);} }
            auto cls = v.inst->klass;
            while (cls) { for (auto& [k,_]:cls->methods) { if(!seen.count(k)){add(k);seen.insert(k);} } cls=cls->parent; }
        } else if (v.is_class()) {
            for (auto& [k,_]:v.cls->methods) add(k);
        } else if (v.is_list()) {
            for (auto& nm : {"length","append","find","pop","insert","sort","as_list"}) add(nm);
        } else if (v.is_str()) {
            for (auto& nm : {"length","lower","upper","strip","find","replace","split","startswith","endswith"}) add(nm);
        } else if (v.is_map()) {
            for (auto& nm : {"get","has","keys","values"}) add(nm);
        } else {
            add("_repr");
        }
        return Value::make_list(list);
    }));

    g.define("list_append", make_native("list_append", 2, [](std::vector<Value> args) {
        if (!args[0].is_list()) throw RuntimeError("list_append: first arg must be list");
        args[0].list->items.push_back(args[1]);
        return Value::make_null();
    }));

    g.define("list_join", make_native("list_join", -1, [this](std::vector<Value> args) {
        if (args.empty() || !args[0].is_list()) throw RuntimeError("list_join: first arg must be list");
        std::string sep = args.size()>1 ? to_str(args[1]) : "";
        std::string result; bool first=true;
        for (auto& item : args[0].list->items) {
            if (!first) result += sep;
            result += to_str(item); first = false;
        }
        return Value::make_str(result);
    }));

    g.define("min", make_native("min", 2, [this](std::vector<Value> args) {
        auto& a = args[0]; auto& b = args[1];
        if (a.is_inst()) {
            auto* m = a.inst->klass->find_method("<");
            if (m) { Value lt = call_method(a,"<",{b},0); return is_truthy(lt) ? a : b; }
        }
        if (a.is_num() && b.is_num()) return a.n <= b.n ? a : b;
        return a < b ? a : b;
    }));

    g.define("max", make_native("max", 2, [this](std::vector<Value> args) {
        auto& a = args[0]; auto& b = args[1];
        if (a.is_inst()) {
            auto* m = a.inst->klass->find_method("<");
            if (m) { Value lt = call_method(a,"<",{b},0); return is_truthy(lt) ? b : a; }
        }
        if (b.is_inst()) {
            auto* m = b.inst->klass->find_method("<");
            if (m) { Value lt = call_method(b,"<",{a},0); return is_truthy(lt) ? a : b; }
        }
        if (a.is_num() && b.is_num()) return a.n >= b.n ? a : b;
        return !(a < b) ? a : b;
    }));

    // Fiber constructor
    g.define("Fiber", make_native("Fiber", 1, [this](std::vector<Value> args) {
        if (!args[0].is_callable()) throw RuntimeError("Fiber expects a callable");
        auto fib = std::make_shared<FiberData>();
        fib->fn_val = args[0];
        fib->state = FiberState::CREATED;
        return Value::make_fiber(fib);
    }));

    // Built-in type stubs
    auto make_type_class = [&](const std::string& nm) {
        auto cls = std::make_shared<ClassData>(); cls->name = nm;
        g.define(nm, Value::make_class(cls));
    };
    make_type_class("String");
    make_type_class("List"); make_type_class("Map"); make_type_class("Range");

    // Built-in stub modules
    auto make_stub = [&](const std::string& nm, std::initializer_list<std::pair<std::string,Value>> attrs) {
        auto mod = std::make_shared<ModuleData>(); mod->name = nm;
        for (auto& [k,v] : attrs) mod->attrs[k] = v;
        Value mv = Value::make_mod(mod);
        modules_[nm] = mv;
        g.define(nm, mv);
    };
    make_stub("lang", {{"clock", globals_->get("clock")}});
    make_stub("io", {{"write", make_native("write",-1,[this](std::vector<Value> a){
        for(auto& v:a) out_<<to_str(v); return Value::make_null();
    })}});
    make_stub("time", {{"sleep", make_native("sleep",1,[](std::vector<Value>){ return Value::make_null(); })}});
    make_stub("path", {{"sep", Value::make_str("/")}});
    make_stub("math", {{"pi", Value::make_num(M_PI)}, {"e", Value::make_num(M_E)},
        {"floor", make_native("floor",1,[](std::vector<Value> a){ return Value::make_num(std::floor(a[0].n)); })},
        {"ceil",  make_native("ceil", 1,[](std::vector<Value> a){ return Value::make_num(std::ceil(a[0].n)); })},
        {"sqrt",  make_native("sqrt", 1,[](std::vector<Value> a){ return Value::make_num(std::sqrt(a[0].n)); })},
        {"abs",   make_native("abs",  1,[](std::vector<Value> a){ return Value::make_num(std::abs(a[0].n)); })},
    });
}

// ── Constructor ───────────────────────────────────────────────────────────────

Interpreter::Interpreter(std::string base_dir)
    : base_dir_(std::move(base_dir)) {
    globals_ = std::make_shared<Environment>();
    env_ = globals_;
    register_builtins();
}

// ── run ───────────────────────────────────────────────────────────────────────

void Interpreter::run(const std::vector<StmtPtr>& stmts) {
    // Compile AST → bytecode
    Compiler compiler;
    auto chunk = compiler.compile_script(stmts);

    // Set up VM with globals and modules
    VM vm(flat_globals_, out_, base_dir_, modules_);
    if (flat_globals_.empty()) vm.register_builtins();

    try {
        vm.run(chunk);
    } catch (AssertError& e) { err_ = std::string("AssertionError: ") + e.what(); throw; }
    catch (RuntimeError& e)  { err_ = e.what(); throw; }
}

void Interpreter::exec_block(const std::vector<StmtPtr>& stmts,
                              std::shared_ptr<Environment> new_env) {
    auto saved = env_;
    env_ = std::move(new_env);
    try { for (auto& s : stmts) exec(s); }
    catch (...) { env_ = saved; throw; }
    env_ = saved;
}

// ── exec ──────────────────────────────────────────────────────────────────────

void Interpreter::exec(const StmtPtr& s) {
    if (auto* e = dynamic_cast<ExprStmt*>(s.get())) {
        eval(e->expr); return;
    }
    if (auto* r = dynamic_cast<ReturnStmt*>(s.get())) {
        // Check for TCO: if return is a direct call to tco_fn_
        if (r->value && tco_fn_) {
            if (auto* call = dynamic_cast<CallExpr*>(r->value.get())) {
                Value callee = eval(call->callee);
                if (callee.is_fn() && callee.fn.get() == tco_fn_.get()) {
                    std::vector<Value> args;
                    for (auto& arg : call->args) args.push_back(eval(arg));
                    throw TailCallSignal{callee.fn, std::move(args)};
                }
            }
        }
        Value v = r->value ? eval(r->value) : Value::make_null();
        throw ReturnSignal{v};
    }
    if (dynamic_cast<BreakStmt*>(s.get())) throw BreakSignal{};

    if (auto* f = dynamic_cast<FuncStmt*>(s.get())) {
        auto fn = std::make_shared<FuncData>();
        fn->name = f->name;
        fn->arity_val = (int)f->params.size();
        auto params = f->params;
        auto body = f->body;
        auto closure = env_;
        // Strip docstring
        if (!body.empty()) {
            if (auto* es = dynamic_cast<ExprStmt*>(body[0].get())) {
                if (auto* lit = dynamic_cast<LiteralExpr*>(es->expr.get())) {
                    if (lit->value.is_str()) { fn->docs = lit->value.s; body.erase(body.begin()); }
                }
            }
        }
        std::weak_ptr<FuncData> fn_weak = fn;
        fn->call = [this, params, body, closure, fn_weak](std::vector<Value> initial_args) -> Value {
            auto fn_shared = fn_weak.lock();
            std::vector<Value> cur_args = std::move(initial_args);
            while (true) {
                auto fn_env = std::make_shared<Environment>(closure);
                for (size_t i = 0; i < params.size() && i < cur_args.size(); ++i)
                    fn_env->define(params[i], cur_args[i]);
                auto prev_tco = tco_fn_;
                tco_fn_ = fn_shared;
                try {
                    exec_block(body, fn_env);
                    tco_fn_ = prev_tco;
                    return Value::make_null();
                } catch (ReturnSignal& ret) {
                    tco_fn_ = prev_tco;
                    return ret.value;
                } catch (TailCallSignal& tc) {
                    tco_fn_ = prev_tco;
                    if (fn_shared && tc.fn.get() == fn_shared.get()) {
                        cur_args = std::move(tc.args);
                        continue; // TCO!
                    }
                    throw;
                } catch (...) {
                    tco_fn_ = prev_tco;
                    throw;
                }
            }
        };
        env_->set(f->name, Value::make_fn(fn));
        return;
    }

    if (auto* c = dynamic_cast<ClassStmt*>(s.get())) {
        auto cls = std::make_shared<ClassData>();
        cls->name = c->name; cls->docs = c->docs;
        if (c->parent) {
            Value pv = eval(c->parent);
            if (!pv.is_class()) throw RuntimeError("Parent must be a class");
            cls->parent = pv.cls;
        }
        Value cls_val = Value::make_class(cls);
        env_->set(c->name, cls_val);
        // Execute method definitions
        for (auto& method_stmt : c->methods) {
            if (auto* fs = dynamic_cast<FuncStmt*>(method_stmt.get())) {
                auto fn = std::make_shared<FuncData>();
                fn->name = fs->name; fn->arity_val = (int)fs->params.size();
                auto params = fs->params; auto body = fs->body; auto closure = env_;
                auto cls_ptr = cls;
                if (!body.empty()) {
                    if (auto* es = dynamic_cast<ExprStmt*>(body[0].get())) {
                        if (auto* lit = dynamic_cast<LiteralExpr*>(es->expr.get())) {
                            if (lit->value.is_str()) { fn->docs=lit->value.s; body.erase(body.begin()); }
                        }
                    }
                }
                std::weak_ptr<FuncData> fn_weak = fn;
                fn->call = [this, params, body, closure, cls_ptr, fn_weak](std::vector<Value> initial_args) -> Value {
                    auto fn_shared = fn_weak.lock();
                    std::vector<Value> cur_args = std::move(initial_args);
                    while (true) {
                        auto fn_env = std::make_shared<Environment>(closure);
                        fn_env->define("self", current_self_);
                        fn_env->define("__method__", Value::make_str(fn_shared ? fn_shared->name : ""));
                        for (size_t i=0; i<params.size()&&i<cur_args.size(); ++i)
                            fn_env->define(params[i], cur_args[i]);
                        auto prev_tco = tco_fn_;
                        tco_fn_ = fn_shared;
                        try {
                            exec_block(body, fn_env);
                            tco_fn_ = prev_tco;
                            return Value::make_null();
                        } catch (ReturnSignal& ret) {
                            tco_fn_ = prev_tco;
                            return ret.value;
                        } catch (TailCallSignal& tc) {
                            tco_fn_ = prev_tco;
                            if (fn_shared && tc.fn.get() == fn_shared.get()) {
                                cur_args = std::move(tc.args);
                                continue;
                            }
                            throw;
                        } catch (...) {
                            tco_fn_ = prev_tco;
                            throw;
                        }
                    }
                };
                cls->methods[fs->name] = Value::make_fn(fn);
            } else {
                exec(method_stmt);
            }
        }
        return;
    }

    if (auto* ifs = dynamic_cast<IfStmt*>(s.get())) {
        for (auto& branch : ifs->branches) {
            if (is_truthy(eval(branch.cond))) {
                auto block_env = std::make_shared<Environment>(env_);
                exec_block(branch.body, block_env); return;
            }
        }
        if (!ifs->else_body.empty()) {
            auto block_env = std::make_shared<Environment>(env_);
            exec_block(ifs->else_body, block_env);
        }
        return;
    }

    if (auto* ws = dynamic_cast<WhileStmt*>(s.get())) {
        while (is_truthy(eval(ws->cond))) {
            try { auto be = std::make_shared<Environment>(env_); exec_block(ws->body, be); }
            catch (BreakSignal&) { break; }
        }
        return;
    }

    if (auto* fs = dynamic_cast<ForStmt*>(s.get())) {
        Value iter = eval(fs->iter);
        auto run_body = [&](const Value& loop_var_val) {
            auto iter_env = std::make_shared<Environment>(env_);
            iter_env->define(fs->var, loop_var_val);
            auto block_env = std::make_shared<Environment>(iter_env);
            exec_block(fs->body, block_env);
        };
        try {
            if (iter.is_range()) {
                double a=iter.range.first, b=iter.range.second;
                if (a<=b) for (double i=a;i<b;++i) run_body(Value::make_num(i));
                else      for (double i=a;i>b;--i) run_body(Value::make_num(i));
            } else if (iter.is_list()) {
                auto items = iter.list->items; // copy for safe iteration
                for (auto& item : items) run_body(item);
            } else if (iter.is_map()) {
                auto pairs = iter.map->pairs;
                for (auto& [k,v] : pairs) run_body(k);
            } else if (iter.is_str()) {
                for (char c : iter.s) run_body(Value::make_str(std::string(1,c)));
            } else throw RuntimeError("for: not iterable");
        } catch (BreakSignal&) {}
        return;
    }

    if (auto* is = dynamic_cast<ImportStmt*>(s.get())) {
        auto do_import = [&](const std::string& module_path, const std::string& alias) {
            try {
                Value mod_val = import_module(module_path);
                env_->set(alias, mod_val);
            } catch (RuntimeError& e) {
                // If module is a built-in stub, it's already registered
                try { env_->set(alias, env_->get(module_path)); }
                catch (...) { /* silently ignore missing modules */ }
            }
        };

        if (is->is_from) {
            Value mod_val;
            try { mod_val = import_module(is->module_path); }
            catch (...) {
                // Try as built-in
                try { mod_val = globals_->get(is->module_path); }
                catch (...) { /* ignore */ }
            }
            for (auto& na : is->names) {
                Value v;
                if (!mod_val.is_null()) {
                    try {
                        if (mod_val.is_mod()) {
                            auto it = mod_val.mod->attrs.find(na.name);
                            if (it != mod_val.mod->attrs.end()) v = it->second;
                        } else {
                            v = do_get(mod_val, na.name, 0);
                        }
                    } catch (...) {}
                }
                env_->set(na.alias, v);
            }
        } else {
            do_import(is->module_path, is->alias);
            for (auto& [p, a] : is->extra_imports) do_import(p, a);
        }
        return;
    }

    throw RuntimeError("Unknown statement type");
}

// ── eval ──────────────────────────────────────────────────────────────────────

Value Interpreter::eval(const ExprPtr& e) {
    if (auto* lit = dynamic_cast<LiteralExpr*>(e.get())) return lit->value;
    if (auto* g   = dynamic_cast<GroupingExpr*>(e.get())) return eval(g->expr);

    if (auto* v = dynamic_cast<VariableExpr*>(e.get()))
        return env_->get(v->name.lexeme);

    if (auto* sc = dynamic_cast<StringConcatExpr*>(e.get())) {
        std::string result;
        for (auto& p : sc->parts) result += to_str(eval(p));
        return Value::make_str(result);
    }

    if (auto* a = dynamic_cast<AssignExpr*>(e.get())) {
        Value rhs = eval(a->value);
        if (a->op.type != TT::ASSIGN) {
            // For instance types, try compound operator method first
            try {
                Value cur = env_->get(a->name.lexeme);
                if (cur.is_inst()) {
                    std::string op_name = a->op.lexeme; // e.g. "+="
                    auto* m = cur.inst->klass->find_method(op_name);
                    if (m) {
                        Value result = call_method(cur, op_name, {rhs}, 0);
                        env_->set(a->name.lexeme, result);
                        return result;
                    }
                }
                // For lists, += mutates in place (so shared refs see the change)
                if (cur.is_list() && a->op.type == TT::PLUS_EQ) {
                    if (rhs.is_list()) {
                        for (auto& item : rhs.list->items) cur.list->items.push_back(item);
                    } else {
                        cur.list->items.push_back(rhs);
                    }
                    env_->set(a->name.lexeme, cur);
                    return cur;
                }
                rhs = apply_op(a->op.type, std::move(cur), rhs);
            } catch (RuntimeError& e_) {
                // If cur not found, just assign
                (void)e_;
            }
        }
        env_->set(a->name.lexeme, rhs);
        return rhs;
    }

    if (auto* u = dynamic_cast<UnaryExpr*>(e.get())) {
        Value right = eval(u->right);
        switch (u->op.type) {
        case TT::MINUS: return Value::make_num(-require_num(right,"unary -"));
        case TT::BANG:
        case TT::NOT:
            if (right.is_inst()) {
                auto* m = right.inst->klass->find_method("!self");
                if (m) return call_method(right, "!self", {}, 0);
            }
            return Value::make_bool(!is_truthy(right));
        case TT::TILDE: return Value::make_num((double)(~to_int(right.n)));
        default: break;
        }
        throw RuntimeError("Unknown unary op");
    }

    if (auto* b = dynamic_cast<BinaryExpr*>(e.get())) {
        Value lv = eval(b->left);
        Value rv = eval(b->right);
        return do_binary(lv, b->op, rv);
    }

    if (auto* log = dynamic_cast<LogicalExpr*>(e.get())) {
        Value left = eval(log->left);
        if (log->op.type == TT::OR) { if (is_truthy(left)) return left; return eval(log->right); }
        else { if (!is_truthy(left)) return left; return eval(log->right); }
    }

    if (auto* idx = dynamic_cast<IndexExpr*>(e.get())) {
        Value obj = eval(idx->obj);
        Value i   = eval(idx->idx);
        return do_index(obj, i, idx->bracket.line);
    }

    if (auto* ia = dynamic_cast<IndexAssignExpr*>(e.get())) {
        Value obj = eval(ia->obj);
        Value idx = eval(ia->idx);
        Value rhs = eval(ia->val);
        // For instances, check operator method
        if (obj.is_inst() && ia->op.type != TT::ASSIGN) {
            auto* cur = obj.inst->attrs.find(to_str(idx)) != obj.inst->attrs.end() ?
                        &obj.inst->attrs.at(to_str(idx)) : nullptr;
            if (cur) {
                std::string op_name = ia->op.lexeme;
                auto* m = obj.inst->klass->find_method(op_name);
                if (m) { rhs = call_method(*cur, op_name, {rhs}, 0); ia->op = Token{TT::ASSIGN,"=",0,0}; }
            }
        }
        do_index_assign(obj, idx, ia->op.type, std::move(rhs), 0);
        return do_index(obj, idx, 0);
    }

    if (auto* g = dynamic_cast<GetExpr*>(e.get())) {
        Value obj = eval(g->obj);
        return do_get(obj, g->name.lexeme, g->name.line);
    }

    if (auto* s = dynamic_cast<SetExpr*>(e.get())) {
        Value obj = eval(s->obj);
        // For instance with compound operator, check method
        Value rhs = eval(s->val);
        if (obj.is_inst() && s->op.type != TT::ASSIGN) {
            auto it = obj.inst->attrs.find(s->name.lexeme);
            if (it != obj.inst->attrs.end()) {
                // Only call operator method if the attribute value is itself an instance
                if (it->second.is_inst()) {
                    std::string op_name = s->op.lexeme;
                    auto* m = it->second.inst->klass->find_method(op_name);
                    if (m) {
                        Value result = call_method(it->second, op_name, {rhs}, 0);
                        obj.inst->attrs[s->name.lexeme] = result;
                        return result;
                    }
                }
                rhs = apply_op(s->op.type, it->second, rhs);
            } else {
                rhs = apply_op(s->op.type, Value::make_null(), rhs);
            }
        }
        do_set(obj, s->name.lexeme, TT::ASSIGN, std::move(rhs), s->name.line);
        return do_get(obj, s->name.lexeme, s->name.line);
    }

    if (auto* c = dynamic_cast<CallExpr*>(e.get())) {
        Value callee = eval(c->callee);
        std::vector<Value> args;
        for (auto& arg : c->args) args.push_back(eval(arg));
        return call_value(callee, std::move(args));
    }

    if (auto* fn = dynamic_cast<FnExpr*>(e.get())) {
        auto f = std::make_shared<FuncData>();
        f->name = "<fn>"; f->arity_val = (int)fn->params.size();
        auto params = fn->params; auto body = fn->body; auto closure = env_;
        // Strip docstring
        if (!body.empty()) {
            if (auto* es = dynamic_cast<ExprStmt*>(body[0].get())) {
                if (auto* lit = dynamic_cast<LiteralExpr*>(es->expr.get())) {
                    if (lit->value.is_str()) { f->docs=lit->value.s; body.erase(body.begin()); }
                }
            }
        }
        std::weak_ptr<FuncData> fn_weak = f;
        f->call = [this, params, body, closure, fn_weak](std::vector<Value> initial_args) -> Value {
            auto fn_shared = fn_weak.lock();
            std::vector<Value> cur_args = std::move(initial_args);
            while (true) {
                auto fn_env = std::make_shared<Environment>(closure);
                for (size_t i=0; i<params.size()&&i<cur_args.size(); ++i)
                    fn_env->define(params[i], cur_args[i]);
                auto prev_tco = tco_fn_; tco_fn_ = fn_shared;
                try {
                    exec_block(body, fn_env); tco_fn_=prev_tco; return Value::make_null();
                } catch (ReturnSignal& ret) { tco_fn_=prev_tco; return ret.value; }
                catch (TailCallSignal& tc) {
                    tco_fn_=prev_tco;
                    if (fn_shared && tc.fn.get()==fn_shared.get()) { cur_args=std::move(tc.args); continue; }
                    throw;
                } catch (...) { tco_fn_=prev_tco; throw; }
            }
        };
        return Value::make_fn(f);
    }

    if (auto* le = dynamic_cast<ListExpr*>(e.get())) {
        auto ld = std::make_shared<ListData>();
        for (auto& elem : le->elements) ld->items.push_back(eval(elem));
        return Value::make_list(ld);
    }

    if (auto* me = dynamic_cast<MapExpr*>(e.get())) {
        auto md = std::make_shared<MapData>();
        for (auto& [k,v] : me->pairs) md->set(eval(k), eval(v));
        return Value::make_map(md);
    }

    if (auto* se = dynamic_cast<SelfExpr*>(e.get())) {
        try { return env_->get("self"); } catch(...) {}
        return current_self_;
    }

    if (auto* sup = dynamic_cast<SuperExpr*>(e.get())) {
        if (!current_class_) throw RuntimeError("super used outside class");
        auto parent = current_class_->parent;
        if (!parent) throw RuntimeError("No parent class");
        Value self_val;
        try { self_val = env_->get("self"); } catch(...) { self_val = current_self_; }
        // Build a wrapper that calls parent's method with parent context
        auto make_super_call = [&](const std::string& mname) -> Value {
            auto* m = parent->find_method(mname);
            if (!m) throw RuntimeError("Parent has no method '"+mname+"'");
            auto parent_ptr = parent;
            auto fn_copy = std::make_shared<FuncData>(*m->fn);
            auto orig_call = fn_copy->call;
            fn_copy->call = [this, orig_call, parent_ptr, self_val](std::vector<Value> args) -> Value {
                auto sc=current_class_; auto ss=current_self_;
                current_class_=parent_ptr; current_self_=self_val;
                Value r;
                try { r=orig_call(args); } catch(ReturnSignal&rs){r=rs.value;}
                catch(...){current_class_=sc;current_self_=ss;throw;}
                current_class_=sc; current_self_=ss;
                return r;
            };
            auto bm = std::make_shared<BoundMethodData>();
            bm->self_val = self_val; bm->fn = fn_copy;
            return Value::make_bound(bm);
        };
        if (!sup->method.empty()) return make_super_call(sup->method);
        // super() without method name: look up current method
        std::string cur_method;
        try { cur_method = env_->get("__method__").s; } catch(...) {}
        if (!cur_method.empty()) return make_super_call(cur_method);
        throw RuntimeError("Cannot determine current method for super()");
    }

    if (auto* ie = dynamic_cast<IsExpr*>(e.get())) {
        Value obj = eval(ie->obj);
        Value cls = eval(ie->cls);
        if (!cls.is_class()) return Value::make_bool(false);
        if (obj.is_inst()) {
            auto klass = obj.inst->klass;
            while (klass) { if (klass.get()==cls.cls.get()) return Value::make_bool(true); klass=klass->parent; }
            return Value::make_bool(false);
        }
        auto& cname = cls.cls->name;
        if (cname=="String") return Value::make_bool(obj.is_str());
        if (cname=="Number") return Value::make_bool(obj.is_num());
        if (cname=="List")   return Value::make_bool(obj.is_list());
        if (cname=="Map")    return Value::make_bool(obj.is_map());
        if (cname=="Range")  return Value::make_bool(obj.is_range());
        return Value::make_bool(false);
    }

    if (auto* ine = dynamic_cast<InExpr*>(e.get())) {
        Value needle = eval(ine->needle);
        Value hay    = eval(ine->haystack);
        bool found = false;
        if (hay.is_list()) { for (auto& item : hay.list->items) if (item==needle){found=true;break;} }
        else if (hay.is_map()) { found = hay.map->has(needle); }
        else if (hay.is_str() && needle.is_str()) { found = hay.s.find(needle.s)!=std::string::npos; }
        else throw RuntimeError("'in' not supported for these types");
        return Value::make_bool(ine->negated ? !found : found);
    }

    if (auto* ye = dynamic_cast<YieldExpr*>(e.get())) {
        Value v = ye->value ? eval(ye->value) : Value::make_null();
        if (current_fiber_) {
            // Thread-based yield: signal caller and wait
            auto* fib = current_fiber_;
            {
                std::lock_guard<std::mutex> lk(fib->mu);
                fib->recv_val = v;
                fib->state = FiberState::SUSPENDED;
                // Save fiber's interpreter state, restore caller's
                fib->fiber_env   = env_;
                fib->fiber_class = current_class_;
                fib->fiber_self  = current_self_;
                env_           = fib->caller_env;
                current_class_ = fib->caller_class;
                current_self_  = fib->caller_self;
                fib->caller_ready = true;
            }
            fib->caller_cv.notify_one();
            // Wait for resume
            {
                std::unique_lock<std::mutex> lk(fib->mu);
                fib->fiber_cv.wait(lk, [fib]{ return fib->fiber_ready || fib->terminated; });
                fib->fiber_ready = false;
                // Restore fiber's interpreter state
                env_           = fib->fiber_env;
                current_class_ = fib->fiber_class;
                current_self_  = fib->fiber_self;
            }
            if (fib->terminated) throw RuntimeError("Fiber terminated");
            return fib->send_val;
        }
        throw YieldSignal{v};
    }

    throw RuntimeError("Unknown expression type");
}

// ── Binary operations ─────────────────────────────────────────────────────────

Value Interpreter::apply_op(TT op, Value lhs, Value rhs) {
    switch(op) {
    case TT::PLUS_EQ:        return do_binary(lhs, Token{TT::PLUS,"+",0,0}, rhs);
    case TT::MINUS_EQ:       return do_binary(lhs, Token{TT::MINUS,"-",0,0}, rhs);
    case TT::STAR_EQ:        return do_binary(lhs, Token{TT::STAR,"*",0,0}, rhs);
    case TT::SLASH_EQ:       return do_binary(lhs, Token{TT::SLASH,"/",0,0}, rhs);
    case TT::PERCENT_EQ:     return do_binary(lhs, Token{TT::PERCENT,"%",0,0}, rhs);
    case TT::STAR_STAR_EQ:   return do_binary(lhs, Token{TT::STAR_STAR,"**",0,0}, rhs);
    case TT::AMP_EQ:         return do_binary(lhs, Token{TT::AMP,"&",0,0}, rhs);
    case TT::PIPE_EQ:        return do_binary(lhs, Token{TT::PIPE,"|",0,0}, rhs);
    case TT::CARET_EQ:       return do_binary(lhs, Token{TT::CARET,"^",0,0}, rhs);
    case TT::LSHIFT_EQ:      return do_binary(lhs, Token{TT::LSHIFT,"<<",0,0}, rhs);
    case TT::RSHIFT_EQ:      return do_binary(lhs, Token{TT::RSHIFT,">>",0,0}, rhs);
    default: return rhs;
    }
}

Value Interpreter::do_binary(const Value& l, const Token& op, const Value& r) {
    auto t = op.type;
    // Equality
    if (t==TT::EQ_EQ) {
        if (l.is_inst()) { auto*m=l.inst->klass->find_method("=="); if(m) return call_method(const_cast<Value&>(l),"==",{r},op.line); }
        return Value::make_bool(l==r);
    }
    if (t==TT::BANG_EQ) {
        if (l.is_inst()) { auto*m=l.inst->klass->find_method("=="); if(m){ Value eq=call_method(const_cast<Value&>(l),"==",{r},op.line); return Value::make_bool(!is_truthy(eq)); } }
        return Value::make_bool(l!=r);
    }
    // Instance operator overloading
    if (l.is_inst()) {
        std::string on;
        if (t==TT::LT) on="<"; if (t==TT::LT_EQ) on="<=";
        if (t==TT::GT) on=">"; if (t==TT::GT_EQ) on=">=";
        if (t==TT::PLUS) on="+"; if (t==TT::MINUS) on="-";
        if (t==TT::STAR) on="*"; if (t==TT::SLASH) on="/";
        if (t==TT::PERCENT) on="%"; if (t==TT::STAR_STAR) on="**";
        if (t==TT::LSHIFT) on="<<"; if (t==TT::RSHIFT) on=">>";
        if (!on.empty()) { auto*m=l.inst->klass->find_method(on); if(m) return call_method(const_cast<Value&>(l),on,{r},op.line); }
    }
    // Range
    if (t==TT::DOT_DOT) {
        if (l.is_num()&&r.is_num()) return Value::make_range(l.n,r.n);
        return Value::make_str(to_str(l)+to_str(r));
    }
    // Plus: number, list concat, string
    if (t==TT::PLUS) {
        if (l.is_num()&&r.is_num()) return Value::make_num(l.n+r.n);
        if (l.is_list()||r.is_list()) {
            auto ld=std::make_shared<ListData>();
            if (l.is_list()) for (auto& i:l.list->items) ld->items.push_back(i);
            if (r.is_list()) for (auto& i:r.list->items) ld->items.push_back(i);
            return Value::make_list(ld);
        }
        return Value::make_str(to_str(l)+to_str(r));
    }
    if (t==TT::MINUS) return Value::make_num(require_num(l,"'-'")-require_num(r,"'-'"));
    if (t==TT::STAR) {
        if (l.is_num()&&r.is_num()) return Value::make_num(l.n*r.n);
        if (l.is_str()&&r.is_num()) { std::string res; for(int i=0;i<(int)r.n;++i) res+=l.s; return Value::make_str(res); }
        if (r.is_str()&&l.is_num()) { std::string res; for(int i=0;i<(int)l.n;++i) res+=r.s; return Value::make_str(res); }
    }
    if (t==TT::SLASH) return Value::make_num(require_num(l,"'/'") / require_num(r,"'/'"));
    if (t==TT::PERCENT) return Value::make_num(std::fmod(require_num(l,"'%'"),require_num(r,"'%'")));
    if (t==TT::STAR_STAR) return Value::make_num(std::pow(require_num(l,"'**'"),require_num(r,"'**'")));
    if (t==TT::LT)    return Value::make_bool(require_num(l,"'<'")<require_num(r,"'<'"));
    if (t==TT::LT_EQ) return Value::make_bool(require_num(l,"'<='")<= require_num(r,"'<='"));
    if (t==TT::GT)    return Value::make_bool(require_num(l,"'>'")<require_num(r,"'>'") ? false : l.n>r.n);
    if (t==TT::GT_EQ) return Value::make_bool(require_num(l,"'>='")>=require_num(r,"'>='"));
    if (t==TT::PIPE)   return Value::make_num((double)(to_int(require_num(l,"|"))|to_int(require_num(r,"|"))));
    if (t==TT::AMP)    return Value::make_num((double)(to_int(require_num(l,"&"))&to_int(require_num(r,"&"))));
    if (t==TT::CARET)  return Value::make_num((double)(to_int(require_num(l,"^"))^to_int(require_num(r,"^"))));
    if (t==TT::LSHIFT) return Value::make_num((double)(to_int(require_num(l,"<<"))<<(int)require_num(r,"<<")));
    if (t==TT::RSHIFT) return Value::make_num((double)(to_int(require_num(l,">>"))>>(int)require_num(r,">>")));
    throw RuntimeError("Unknown binary operator: "+op.lexeme);
}

// ── Indexing ──────────────────────────────────────────────────────────────────

Value Interpreter::do_index(const Value& obj, const Value& idx, int line) {
    if (obj.is_list()) {
        if (idx.is_range()) return slice(obj, idx.range.first, idx.range.second, line);
        int i=(int)require_num(idx,"list index");
        if (i<0) i=(int)obj.list->items.size()+i;
        if (i<0||i>=(int)obj.list->items.size()) throw RuntimeError("List index out of range");
        return obj.list->items[i];
    }
    if (obj.is_str()) {
        if (idx.is_range()) return slice(obj, idx.range.first, idx.range.second, line);
        int i=(int)require_num(idx,"string index");
        if (i<0) i=(int)obj.s.size()+i;
        if (i<0||i>=(int)obj.s.size()) throw RuntimeError("String index out of range");
        return Value::make_str(std::string(1,obj.s[i]));
    }
    if (obj.is_map()) {
        auto* v=obj.map->find(idx);
        if (!v) throw RuntimeError("Map key not found: "+to_str(idx));
        return *v;
    }
    if (obj.is_mod()) {
        auto it=obj.mod->attrs.find(to_str(idx));
        if (it==obj.mod->attrs.end()) throw RuntimeError("Module has no attribute '"+to_str(idx)+"'");
        return it->second;
    }
    throw RuntimeError("Cannot index this type");
}

void Interpreter::do_index_assign(Value& obj, const Value& idx, TT op, Value rhs, int) {
    if (obj.is_list()) {
        int i=(int)require_num(idx,"list index");
        if (i<0) i=(int)obj.list->items.size()+i;
        if (i<0||i>=(int)obj.list->items.size()) throw RuntimeError("List index out of range");
        if (op!=TT::ASSIGN) rhs=apply_op(op,obj.list->items[i],rhs);
        obj.list->items[i]=std::move(rhs); return;
    }
    if (obj.is_map()) {
        if (op!=TT::ASSIGN) { auto* cur=obj.map->find(idx); if(!cur) throw RuntimeError("Map key not found for compound assign"); rhs=apply_op(op,*cur,rhs); }
        obj.map->set(idx,std::move(rhs)); return;
    }
    throw RuntimeError("Cannot assign to index of this type");
}

Value Interpreter::slice(const Value& obj, double a, double b, int) {
    if (obj.is_str()) {
        const std::string& s=obj.s; int n=(int)s.size();
        int ia=(int)a; if(ia<0)ia=n+ia; ia=std::max(0,std::min(ia,n>0?n-1:0));
        int ib=(int)b; if(ib<0)ib=n+ib;
        if (n==0) return Value::make_str("");
        std::string res;
        if (ia<=ib) { ib=std::min(ib,n-1); for(int i=ia;i<=ib;++i) res+=s[i]; }
        else        { ib=std::max(ib,0);   for(int i=ia;i>=ib;--i) res+=s[i]; }
        return Value::make_str(res);
    }
    if (obj.is_list()) {
        auto& items=obj.list->items; int n=(int)items.size();
        if (n==0) return Value::make_list();
        int ia=(int)a; if(ia<0)ia=n+ia; ia=std::max(0,std::min(ia,n-1));
        int ib=(int)b; if(ib<0)ib=n+ib;
        auto result=std::make_shared<ListData>();
        if (ia<=ib) { ib=std::min(ib,n-1); for(int i=ia;i<=ib;++i) result->items.push_back(items[i]); }
        else        { ib=std::max(ib,0);   for(int i=ia;i>=ib;--i) result->items.push_back(items[i]); }
        return Value::make_list(result);
    }
    throw RuntimeError("slice: not a sliceable type");
}

// ── Attribute access ──────────────────────────────────────────────────────────

Value Interpreter::do_get(const Value& obj, const std::string& name, int line) {
    if (obj.is_inst()) {
        auto it=obj.inst->attrs.find(name);
        if (it!=obj.inst->attrs.end()) return it->second;
        auto* m=obj.inst->klass->find_method(name);
        if (m) {
            auto bm=std::make_shared<BoundMethodData>(); bm->self_val=obj; bm->fn=m->fn;
            return Value::make_bound(bm);
        }
        throw RuntimeError("Instance has no attribute '"+name+"'");
    }
    if (obj.is_class()) {
        auto it=obj.cls->class_attrs.find(name);
        if (it!=obj.cls->class_attrs.end()) return it->second;
        auto* m=obj.cls->find_method(name); if(m) return *m;
        if (name=="_docs") return Value::make_str(obj.cls->docs);
        if (name=="parent") return obj.cls->parent ? Value::make_class(obj.cls->parent) : Value::make_null();
        throw RuntimeError("Class '"+obj.cls->name+"' has no attribute '"+name+"'");
    }
    if (obj.is_mod()) {
        auto it=obj.mod->attrs.find(name);
        if (it!=obj.mod->attrs.end()) return it->second;
        throw RuntimeError("Module has no attribute '"+name+"'");
    }
    if (obj.is_fn()) {
        if (name=="arity") return Value::make_num(obj.fn->arity_val);
        if (name=="name")  return Value::make_str(obj.fn->name);
        if (name=="_docs") return Value::make_str(obj.fn->docs);
        if (name=="bind") {
            auto fn_val=obj;
            return make_native("bind",1,[this,fn_val](std::vector<Value> args)->Value{
                auto bm=std::make_shared<BoundMethodData>(); bm->self_val=args[0]; bm->fn=fn_val.fn;
                return Value::make_bound(bm);
            });
        }
        throw RuntimeError("Function has no attribute '"+name+"'");
    }
    if (obj.is_bound()) {
        if (name=="arity") return Value::make_num(obj.bound->fn->arity_val);
        if (name=="name")  return Value::make_str(obj.bound->fn->name);
        if (name=="_docs") return Value::make_str(obj.bound->fn->docs);
        throw RuntimeError("BoundMethod has no attribute '"+name+"'");
    }
    if (obj.is_fiber()) {
        if (name=="is_done") return Value::make_bool(obj.fiber->state==FiberState::DONE);
        auto fib=obj;
        if (name=="run"||name=="resume") {
            return make_native(name,-1,[this,fib](std::vector<Value> args)->Value{
                return call_value(fib, std::move(args));
            });
        }
        throw RuntimeError("Fiber has no attribute '"+name+"'");
    }
    return get_attr(obj, name, line);
}

void Interpreter::do_set(Value& obj, const std::string& name, TT op, Value rhs, int) {
    if (obj.is_inst()) {
        if (op!=TT::ASSIGN) {
            auto it=obj.inst->attrs.find(name);
            if (it!=obj.inst->attrs.end()) rhs=apply_op(op,it->second,rhs);
        }
        obj.inst->attrs[name]=std::move(rhs); return;
    }
    if (obj.is_class()) {
        if (op!=TT::ASSIGN) {
            auto it=obj.cls->class_attrs.find(name);
            if (it!=obj.cls->class_attrs.end()) rhs=apply_op(op,it->second,rhs);
        }
        obj.cls->class_attrs[name]=std::move(rhs); return;
    }
    if (obj.is_mod()) { obj.mod->attrs[name]=std::move(rhs); return; }
    throw RuntimeError("Cannot set attribute on this type");
}

// ── Type attributes ───────────────────────────────────────────────────────────

Value Interpreter::get_attr(const Value& obj, const std::string& name, int) {
    if (obj.is_str()) {
        if (name=="length") return Value::make_num((double)obj.s.size());
        auto sv=obj;
        auto make_str_method=[&](const std::string& nm, int ar, std::function<Value(std::vector<Value>)> fn) {
            return make_native(nm, ar, std::move(fn));
        };
        if (name=="lower")  return make_str_method("lower",0,[sv](auto){std::string r=sv.s;for(auto&c:r)c=tolower(c);return Value::make_str(r);});
        if (name=="upper")  return make_str_method("upper",0,[sv](auto){std::string r=sv.s;for(auto&c:r)c=toupper(c);return Value::make_str(r);});
        if (name=="strip")  return make_str_method("strip",0,[sv](auto){
            std::string s=sv.s; size_t a=s.find_first_not_of(" \t\n\r");
            if(a==std::string::npos) return Value::make_str("");
            size_t b=s.find_last_not_of(" \t\n\r"); return Value::make_str(s.substr(a,b-a+1));
        });
        if (name=="find")   return make_str_method("find",1,[sv](std::vector<Value> args){
            if(!args[0].is_str()) throw RuntimeError("find: expected string");
            auto p=sv.s.find(args[0].s); return Value::make_num(p==std::string::npos?-1:(double)p);
        });
        if (name=="replace") return make_str_method("replace",-1,[sv](std::vector<Value> args){
            if(args.size()<2) throw RuntimeError("replace: need 2 args");
            std::string s=sv.s,from=args[0].s,to=args[1].s;
            int max_c=args.size()>2?(int)args[2].n:INT_MAX; int cnt=0;
            size_t pos=0; std::string result;
            while((pos=s.find(from,pos))!=std::string::npos&&cnt<max_c){
                result+=s.substr(0,pos); result+=to; s=s.substr(pos+from.size()); pos=0; ++cnt;
            }
            result+=s; return Value::make_str(result);
        });
        if (name=="split")  return make_str_method("split",1,[sv](std::vector<Value> args){
            std::string s=sv.s,delim=args[0].s;
            auto result=std::make_shared<ListData>();
            if(delim.empty()){for(char c:s) result->items.push_back(Value::make_str(std::string(1,c)));}
            else{size_t pos=0,next; while((next=s.find(delim,pos))!=std::string::npos){result->items.push_back(Value::make_str(s.substr(pos,next-pos)));pos=next+delim.size();} result->items.push_back(Value::make_str(s.substr(pos)));}
            return Value::make_list(result);
        });
        if (name=="startswith") return make_str_method("startswith",1,[sv](std::vector<Value> args){
            auto check=[&sv](const std::string&p){return sv.s.substr(0,p.size())==p;};
            if(args[0].is_str()) return Value::make_bool(check(args[0].s));
            if(args[0].is_list()){for(auto&i:args[0].list->items)if(i.is_str()&&check(i.s))return Value::make_bool(true);return Value::make_bool(false);}
            return Value::make_bool(false);
        });
        if (name=="endswith") return make_str_method("endswith",1,[sv](std::vector<Value> args){
            auto check=[&sv](const std::string&s){return sv.s.size()>=s.size()&&sv.s.substr(sv.s.size()-s.size())==s;};
            if(args[0].is_str()) return Value::make_bool(check(args[0].s));
            if(args[0].is_list()){for(auto&i:args[0].list->items)if(i.is_str()&&check(i.s))return Value::make_bool(true);return Value::make_bool(false);}
            return Value::make_bool(false);
        });
        throw RuntimeError("String has no attribute '"+name+"'");
    }
    if (obj.is_list()) {
        if (name=="length") return Value::make_num((double)obj.list->items.size());
        auto lv=obj;
        if (name=="append") return make_native("append",1,[lv](std::vector<Value> args){ lv.list->items.push_back(args[0]); return lv; });
        if (name=="find")   return make_native("find",1,[lv](std::vector<Value> args){ auto&items=lv.list->items; for(int i=0;i<(int)items.size();++i)if(items[i]==args[0])return Value::make_num(i); return Value::make_num(-1); });
        if (name=="pop")    return make_native("pop",-1,[lv](std::vector<Value> args){
            auto&items=lv.list->items; if(items.empty())throw RuntimeError("pop: empty list");
            int idx=args.empty()?(int)items.size()-1:(int)args[0].n;
            if(idx<0)idx=(int)items.size()+idx;
            if(idx<0||idx>=(int)items.size())throw RuntimeError("pop: index out of range");
            Value v=items[idx]; items.erase(items.begin()+idx); return v;
        });
        if (name=="insert") return make_native("insert",2,[lv](std::vector<Value> args){
            auto&items=lv.list->items; int idx=(int)args[0].n;
            if(idx<0)idx=(int)items.size()+idx;
            idx=std::max(0,std::min(idx,(int)items.size()));
            items.insert(items.begin()+idx,args[1]); return Value::make_null();
        });
        if (name=="sort") return make_native("sort",-1,[this,lv](std::vector<Value> args){
            auto&items=lv.list->items;
            if(!args.empty()&&args[0].is_callable()){
                std::sort(items.begin(),items.end(),[this,&args](const Value&a,const Value&b){
                    Value r=call_value(args[0],{a,b}); return is_truthy(r);
                });
            } else {
                std::sort(items.begin(),items.end(),[](const Value&a,const Value&b){ return a<b; });
            }
            return lv;
        });
        if (name=="as_list") return lv;
        if (name=="keys") { auto md=std::make_shared<ListData>(); for(auto&i:lv.list->items)md->items.push_back(i); return Value::make_list(md); }
        throw RuntimeError("List has no attribute '"+name+"'");
    }
    if (obj.is_map()) {
        auto mv=obj;
        if (name=="get") return make_native("get",-1,[mv](std::vector<Value> args){
            if(args.empty())throw RuntimeError("get: need key");
            auto*v=mv.map->find(args[0]); if(!v) return args.size()>1?args[1]:Value::make_null(); return *v;
        });
        if (name=="has") return make_native("has",1,[mv](std::vector<Value> args){ return Value::make_bool(mv.map->has(args[0])); });
        if (name=="keys") return make_native("keys",0,[mv](auto){ auto ld=std::make_shared<ListData>(); for(auto&[k,v]:mv.map->pairs) ld->items.push_back(k); return Value::make_list(ld); });
        if (name=="values") return make_native("values",0,[mv](auto){ auto ld=std::make_shared<ListData>(); for(auto&[k,v]:mv.map->pairs) ld->items.push_back(v); return Value::make_list(ld); });
        throw RuntimeError("Map has no attribute '"+name+"'");
    }
    if (obj.is_range()) {
        if (name=="first") return Value::make_num(obj.range.first);
        if (name=="last")  return Value::make_num(obj.range.second);
        if (name=="as_list") {
            auto ld=std::make_shared<ListData>(); double a=obj.range.first,b=obj.range.second;
            if(a<=b) for(double i=a;i<b;++i) ld->items.push_back(Value::make_num(i));
            else     for(double i=a;i>b;--i) ld->items.push_back(Value::make_num(i));
            return Value::make_list(ld);
        }
        throw RuntimeError("Range has no attribute '"+name+"'");
    }
    if (obj.is_num()) {
        if (name=="times") {
            auto nv=obj;
            return make_native("times",1,[this,nv](std::vector<Value> args)->Value{
                int n=(int)nv.n;
                for(int i=0;i<n;++i) call_value(args[0],{Value::make_num(i)});
                return Value::make_null();
            });
        }
        throw RuntimeError("Number has no attribute '"+name+"'");
    }
    if (obj.is_null()) {
        if (name=="_repr") return Value::make_str("null");
        throw RuntimeError("null has no attribute '"+name+"'");
    }
    throw RuntimeError("Value has no attribute '"+name+"'");
}

Value Interpreter::call_method(Value self, const std::string& method, std::vector<Value> args, int) {
    if (self.is_inst()) {
        auto* m=self.inst->klass->find_method(method);
        if (!m) throw RuntimeError("No method '"+method+"' on "+self.inst->klass->name);
        auto sc=current_class_; auto ss=current_self_;
        current_class_=self.inst->klass; current_self_=self;
        Value result;
        try { result=m->fn->call(args); }
        catch (ReturnSignal& r){ result=r.value; }
        catch(...){ current_class_=sc; current_self_=ss; throw; }
        current_class_=sc; current_self_=ss;
        return result;
    }
    Value method_val=get_attr(self,method,0);
    return call_value(method_val,std::move(args));
}

// ── call_value ────────────────────────────────────────────────────────────────

Value Interpreter::call_value(const Value& callee, std::vector<Value> args) {
    if (callee.is_fn()) {
        auto& f=*callee.fn;
        if (f.arity_val>=0&&(int)args.size()!=f.arity_val)
            throw RuntimeError("Expected "+std::to_string(f.arity_val)+" args, got "+std::to_string(args.size()));
        return f.call(std::move(args));
    }
    if (callee.is_bound()) {
        auto& bm=*callee.bound;
        auto ss=current_self_; auto sc=current_class_;
        current_self_=bm.self_val;
        if (bm.self_val.is_inst()) current_class_=bm.self_val.inst->klass;
        Value result;
        try { result=bm.fn->call(std::move(args)); }
        catch (ReturnSignal& r){ result=r.value; }
        catch(...){ current_self_=ss; current_class_=sc; throw; }
        current_self_=ss; current_class_=sc;
        return result;
    }
    if (callee.is_class()) {
        auto inst=std::make_shared<InstanceData>(); inst->klass=callee.cls;
        Value inst_val=Value::make_inst(inst);
        auto ss=current_self_; auto sc=current_class_;
        current_self_=inst_val; current_class_=callee.cls;
        auto* init=callee.cls->find_method("_init");
        if (init) {
            try { init->fn->call(std::move(args)); }
            catch(ReturnSignal&){}
            catch(...){ current_self_=ss; current_class_=sc; throw; }
        }
        current_self_=ss; current_class_=sc;
        return inst_val;
    }
    if (callee.is_fiber()) {
        auto& fib=*callee.fiber;
        if (fib.state==FiberState::DONE) throw RuntimeError("Fiber is done");
        auto impl_ptr = std::static_pointer_cast<FiberImpl>(fib.impl);
        if (fib.state==FiberState::CREATED) {
            // Start fiber in new thread
            auto new_impl=std::make_shared<FiberImpl>();
            fib.impl=new_impl;
            auto fib_ptr=callee.fiber;
            auto fn_val=fib.fn_val;
            std::vector<Value> run_args=std::move(args);

            // Save caller's interpreter state
            new_impl->caller_env   = env_;
            new_impl->caller_class = current_class_;
            new_impl->caller_self  = current_self_;

            // Fiber thread runs on THIS interpreter
            new_impl->thread = std::thread([this, fib_ptr, fn_val, run_args=std::move(run_args), impl=new_impl.get()]() mutable {
                auto prev_fiber = current_fiber_;
                current_fiber_ = impl;
                Value result;
                try {
                    result = call_value(fn_val, run_args);
                } catch (...) {
                    current_fiber_ = prev_fiber;
                    // Restore caller state
                    env_           = impl->caller_env;
                    current_class_ = impl->caller_class;
                    current_self_  = impl->caller_self;
                    std::lock_guard<std::mutex> lk(impl->mu);
                    impl->exc_ptr = std::current_exception();
                    impl->recv_val = Value::make_null();
                    fib_ptr->state = FiberState::DONE;
                    impl->state = FiberState::DONE;
                    impl->caller_ready = true;
                    impl->caller_cv.notify_one();
                    return;
                }
                current_fiber_ = prev_fiber;
                // Restore caller state
                env_           = impl->caller_env;
                current_class_ = impl->caller_class;
                current_self_  = impl->caller_self;
                std::lock_guard<std::mutex> lk(impl->mu);
                impl->recv_val = result;
                fib_ptr->state = FiberState::DONE;
                impl->state = FiberState::DONE;
                impl->caller_ready = true;
                impl->caller_cv.notify_one();
            });
            fib.state = FiberState::RUNNING;

            // Wait for first yield or completion
            std::unique_lock<std::mutex> lk(new_impl->mu);
            new_impl->caller_cv.wait(lk, [&]{ return new_impl->caller_ready; });
            new_impl->caller_ready = false;
            fib.state = new_impl->state;
            if (new_impl->exc_ptr) std::rethrow_exception(new_impl->exc_ptr);
            return new_impl->recv_val;
        } else {
            // SUSPENDED → resume
            if (!impl_ptr) throw RuntimeError("Fiber not initialized");
            Value resume_val = args.empty() ? Value::make_null() : args[0];
            {
                std::lock_guard<std::mutex> lk(impl_ptr->mu);
                impl_ptr->send_val = resume_val;
                // Save caller state, fiber will restore it after yield
                impl_ptr->caller_env   = env_;
                impl_ptr->caller_class = current_class_;
                impl_ptr->caller_self  = current_self_;
                fib.state = FiberState::RUNNING;
                impl_ptr->state = FiberState::RUNNING;
                impl_ptr->fiber_ready = true;
            }
            impl_ptr->fiber_cv.notify_one();
            // Wait for next yield or completion
            std::unique_lock<std::mutex> lk(impl_ptr->mu);
            impl_ptr->caller_cv.wait(lk, [&]{ return impl_ptr->caller_ready; });
            impl_ptr->caller_ready = false;
            fib.state = impl_ptr->state;
            if (impl_ptr->exc_ptr) std::rethrow_exception(impl_ptr->exc_ptr);
            return impl_ptr->recv_val;
        }
    }
    throw RuntimeError("Value is not callable");
}

// ── Import ────────────────────────────────────────────────────────────────────

Value Interpreter::import_module(const std::string& path, const std::string& /*from_file*/) {
    // Check cache by original path (covers built-ins and already-cached modules)
    auto it=modules_.find(path); if (it!=modules_.end()) return it->second;
    // Built-ins
    static const std::string builtins[] = {"lang","io","time","path","math"};
    for (auto& b : builtins) if (path==b) {
        // Return stub
        auto mod=std::make_shared<ModuleData>(); mod->name=path;
        return Value::make_mod(mod);
    }

    // Resolve file path
    // path like "basics" → "basics.pk"
    // path like "imports.fns" → "imports/fns.pk"
    // path like "^basics" → "../basics.pk"
    // path like "^^functions" → "../../functions.pk" 
    std::string rel = path;
    // Handle ^^ prefix
    std::string up_prefix;
    while (!rel.empty() && rel[0]=='^') { up_prefix += "../"; rel = rel.substr(1); }
    // Replace dots with /
    for (char& c : rel) if (c=='.') c='/';
    rel += ".pk";

    // Search paths: base_dir
    std::vector<std::string> search_paths;
    if (!base_dir_.empty()) {
        search_paths.push_back(base_dir_ + "/" + up_prefix + rel);
        // Also try _init.pk version
        std::string init_path = base_dir_ + "/" + up_prefix + rel.substr(0,rel.size()-3) + "/_init.pk";
        search_paths.push_back(init_path);
    }
    // Try relative to current dir
    search_paths.push_back(up_prefix + rel);

    std::string full_path;
    for (auto& p : search_paths) if (fs::exists(p)) { full_path=p; break; }
    if (full_path.empty()) throw RuntimeError("Module not found: "+path);

    // Normalize to canonical path for cycle detection across different import paths
    std::string cache_key;
    try { cache_key = fs::canonical(full_path).string(); }
    catch (...) { cache_key = full_path; }

    // Check by canonical path (handles "cyclic_a" vs "imports.cyclic_a" for same file)
    auto it2 = modules_.find(cache_key);
    if (it2 != modules_.end()) {
        modules_[path] = it2->second; // also cache by original path
        return it2->second;
    }

    // Create module stub in cache FIRST (for cyclic imports)
    auto mod=std::make_shared<ModuleData>(); mod->name=path; mod->path=full_path;
    Value mod_val=Value::make_mod(mod);
    modules_[cache_key]=mod_val;
    modules_[path]=mod_val;

    exec_module(full_path, *mod);
    return mod_val;
}

void Interpreter::exec_module(const std::string& path, ModuleData& mod) {
    std::ifstream f(path);
    if (!f) throw RuntimeError("Cannot open module file: "+path);
    std::ostringstream ss; ss<<f.rdbuf(); std::string src=ss.str();

    std::string mod_base=fs::path(path).parent_path().string();
    if (mod_base.empty()) mod_base=".";

    // Create a module-level environment (copy of globals, no parent).
    // No parent means undefined vars at global scope return null (pocketlang semantics).
    auto mod_env = std::make_shared<Environment>();
    mod_env->vars = globals_->vars;  // copy all built-ins

    // Save interpreter state
    auto saved_env      = env_;
    auto saved_class    = current_class_;
    auto saved_self     = current_self_;
    auto saved_tco      = tco_fn_;
    auto saved_base     = base_dir_;
    auto saved_fiber    = current_fiber_;

    env_            = mod_env;
    base_dir_       = mod_base;
    current_class_  = nullptr;
    current_self_   = Value::make_null();
    tco_fn_         = nullptr;
    current_fiber_  = nullptr;

    auto restore = [&]() {
        env_            = saved_env;
        current_class_  = saved_class;
        current_self_   = saved_self;
        tco_fn_         = saved_tco;
        base_dir_       = saved_base;
        current_fiber_  = saved_fiber;
    };

    Lexer lex(src);
    auto tokens=lex.tokenize();
    Parser parser(std::move(tokens));
    auto stmts=parser.parse();

    try {
        for (auto& stmt : stmts) exec(stmt);
    } catch (ReturnSignal&) {
        // top-level return in a module is OK
    } catch (...) {
        restore();
        throw;
    }
    restore();

    // Export module's non-builtin names to mod.attrs
    static const std::set<std::string> skip_set = {
        "print","assert","str","type","hex","Number","clock","dir",
        "list_append","list_join","min","max","Fiber",
        "String","List","Map","Range","lang","io","time","path","math",
        "__method__"
    };
    for (auto& [name, val] : mod_env->vars) {
        if (!skip_set.count(name)) mod.attrs[name] = val;
    }
}

} // namespace pocketpp
