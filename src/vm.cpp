#include "vm.hpp"
#include "compiler.hpp"
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
#include <set>
#include <sstream>
#include <stdexcept>

namespace pocketpp {

namespace fs = std::filesystem;

// ── FiberImpl (thread-based) ──────────────────────────────────────────────────
struct FiberImpl {
    std::thread thread;
    std::mutex mu;
    std::condition_variable caller_cv;
    std::condition_variable fiber_cv;

    bool caller_ready{false};
    bool fiber_ready{false};
    bool terminated{false};

    FiberState state{FiberState::CREATED};

    Value send_val;
    Value recv_val;
    std::exception_ptr exc_ptr;

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

// ── Constructor ───────────────────────────────────────────────────────────────

VM::VM(std::unordered_map<std::string, Value>& globals,
       std::ostringstream& out,
       const std::string& base_dir,
       std::unordered_map<std::string, Value>& modules)
    : globals_(globals), out_(out), base_dir_(base_dir), modules_(modules)
{
    stack_.reserve(STACK_MAX);
}

// ── Stack helpers ─────────────────────────────────────────────────────────────

void VM::push(Value v) {
    if (stack_.size() >= STACK_MAX) throw RuntimeError("Stack overflow");
    stack_.push_back(std::move(v));
}
Value VM::pop() {
    if (stack_.empty()) throw RuntimeError("Stack underflow");
    Value v = std::move(stack_.back());
    stack_.pop_back();
    return v;
}
Value& VM::peek(size_t dist) {
    return stack_[stack_.size() - 1 - dist];
}

// ── Frame/byte reading ────────────────────────────────────────────────────────

uint8_t VM::read_byte() {
    return frame().chunk->code[frame().ip++];
}
uint16_t VM::read_u16() {
    uint8_t hi = read_byte(), lo = read_byte();
    return (uint16_t)((hi << 8) | lo);
}
int VM::line() {
    size_t ip = frame().ip > 0 ? frame().ip - 1 : 0;
    auto& lines = frame().chunk->lines;
    if (ip < lines.size()) return lines[ip];
    return 0;
}

// ── Value helpers ─────────────────────────────────────────────────────────────

bool VM::is_truthy(const Value& v) {
    if (v.is_null()) return false;
    if (v.is_bool()) return v.b;
    return true;
}

std::string VM::to_str(const Value& v, std::vector<const void*>* seen) {
    switch (v.type) {
    case Value::Type::Null:    return "null";
    case Value::Type::Bool:    return v.b ? "true" : "false";
    case Value::Type::Number: {
        double d = v.n;
        long long i = (long long)d;
        if ((double)i == d && !std::isinf(d)) return std::to_string(i);
        std::ostringstream oss; oss << d; return oss.str();
    }
    case Value::Type::String:  return v.s;
    case Value::Type::Range: {
        auto fmt=[](double d){ long long i=(long long)d; return (double)i==d?std::to_string(i):[&]{std::ostringstream o;o<<d;return o.str();}(); };
        return fmt(v.range.first)+".."+fmt(v.range.second);
    }
    case Value::Type::Function: return "<fn "+v.fn->name+">";
    case Value::Type::BoundMethod: return "<bound "+v.bound->fn->name+">";
    case Value::Type::Class:   return "<class "+v.cls->name+">";
    case Value::Type::Module:  return "<module "+v.mod->name+">";
    case Value::Type::Fiber: {
        const char* states[] = {"created","running","suspended","done"};
        return std::string("<fiber ") + states[std::min((int)v.fiber->state,3)] + ">";
    }
    case Value::Type::Instance: {
        auto& inst = *v.inst;
        auto* method = inst.klass->find_method("_str");
        if (!method) method = inst.klass->find_method("_repr");
        if (method && method->is_fn()) {
            Value result;
            try { result = call_method(const_cast<Value&>(v), "_str", {}); }
            catch (...) {
                try { result = call_method(const_cast<Value&>(v), "_repr", {}); }
                catch (...) {}
            }
            if (!result.is_null()) return to_str(result);
        }
        return "<"+inst.klass->name+" instance>";
    }
    case Value::Type::List: {
        std::vector<const void*> local_seen;
        auto* ls = seen ? seen : &local_seen;
        const void* ptr = v.list.get();
        for (auto p : *ls) if (p == ptr) return "[...]";
        ls->push_back(ptr);
        std::string r = "[";
        for (size_t i = 0; i < v.list->items.size(); ++i) {
            if (i > 0) r += ", ";
            r += to_str(v.list->items[i], ls);
        }
        ls->pop_back();
        return r + "]";
    }
    case Value::Type::Map: {
        std::vector<const void*> local_seen;
        auto* ls = seen ? seen : &local_seen;
        const void* ptr = v.map.get();
        for (auto p : *ls) if (p == ptr) return "{...}";
        ls->push_back(ptr);
        std::string r = "{";
        for (size_t i = 0; i < v.map->pairs.size(); ++i) {
            if (i > 0) r += ", ";
            auto& [k, val] = v.map->pairs[i];
            r += "\""+to_str(k)+"\":"+to_str(val, ls);
        }
        ls->pop_back();
        return r + "}";
    }
    }
    return "?";
}

Value VM::make_native(const std::string& nm, int ar,
                      std::function<Value(std::vector<Value>)> fn) {
    auto f = std::make_shared<FuncData>();
    f->name = nm; f->arity_val = ar; f->call = std::move(fn);
    return Value::make_fn(f);
}

// ── Upvalue management ────────────────────────────────────────────────────────

std::shared_ptr<UpvalueSlot> VM::capture_upvalue(size_t abs_slot) {
    // Reuse existing open upvalue for this slot
    for (auto& uv : open_upvalues_)
        if (uv->location == &stack_[abs_slot]) return uv;
    auto uv = std::make_shared<UpvalueSlot>(&stack_[abs_slot]);
    open_upvalues_.push_back(uv);
    return uv;
}

void VM::close_upvalues(size_t min_slot) {
    for (auto it = open_upvalues_.begin(); it != open_upvalues_.end(); ) {
        if ((*it)->location >= &stack_[0] + min_slot) {
            (*it)->close_over();
            it = open_upvalues_.erase(it);
        } else {
            ++it;
        }
    }
}

// ── Call helpers ──────────────────────────────────────────────────────────────

void VM::do_call(const Value& callee, int argc, bool is_tail) {
    if (callee.is_fn()) {
        auto& f = *callee.fn;
        if (f.arity_val >= 0 && argc != f.arity_val)
            throw RuntimeError("Expected "+std::to_string(f.arity_val)+" args, got "+std::to_string(argc));
        if (f.is_native()) {
            // Native: collect args, call, push result
            std::vector<Value> args(stack_.end()-argc, stack_.end());
            stack_.erase(stack_.end()-argc-1, stack_.end()); // remove callee+args
            push(f.call(std::move(args)));
            return;
        }
        // Bytecode call
        if (is_tail && !frames_.empty()) {
            // Tail call: reuse current frame
            close_upvalues(frames_.back().base);
            size_t base = frames_.back().base;
            // callee is at base-1, args are at end of stack
            size_t args_start = stack_.size() - argc;
            stack_[base - 1] = callee;
            for (int i = 0; i < argc; ++i)
                stack_[base + i] = stack_[args_start + i];
            stack_.resize(base + argc);
            frames_.back().fn    = callee.fn;
            frames_.back().chunk = callee.fn->chunk;
            frames_.back().ip    = 0;
            frames_.back().self_val  = Value::make_null();
            frames_.back().class_ptr = nullptr;
            return;
        }
        size_t base = stack_.size() - argc;
        CallFrame fr;
        fr.fn    = callee.fn;
        fr.chunk = callee.fn->chunk;
        fr.ip    = 0;
        fr.base  = base;
        fr.self_val  = Value::make_null();
        fr.class_ptr = nullptr;
        frames_.push_back(std::move(fr));
        return;
    }
    if (callee.is_bound()) {
        auto& bm = *callee.bound;
        if (bm.fn->arity_val >= 0 && argc != bm.fn->arity_val)
            throw RuntimeError("Expected "+std::to_string(bm.fn->arity_val)+" args, got "+std::to_string(argc));
        if (bm.fn->is_native()) {
            std::vector<Value> args(stack_.end()-argc, stack_.end());
            stack_.erase(stack_.end()-argc-1, stack_.end());
            // Native bound: call with self context
            // For native bound methods, call is just f.call(args)
            push(bm.fn->call(std::move(args)));
            return;
        }
        size_t base = stack_.size() - argc;
        CallFrame fr;
        fr.fn    = bm.fn;
        fr.chunk = bm.fn->chunk;
        fr.ip    = 0;
        fr.base  = base;
        fr.self_val  = bm.self_val;
        fr.class_ptr = bm.self_val.is_inst() ? bm.self_val.inst->klass : nullptr;
        frames_.push_back(std::move(fr));
        return;
    }
    if (callee.is_class()) {
        // Save args before erasing (we need them for _init)
        std::vector<Value> args_saved(stack_.end()-argc, stack_.end());
        // Create the new instance
        auto inst = std::make_shared<InstanceData>();
        inst->klass = callee.cls;
        Value inst_val = Value::make_inst(inst);
        // Remove callee + args from stack
        stack_.erase(stack_.end()-argc-1, stack_.end());

        auto* init = callee.cls->find_method("_init");
        if (init && init->is_fn()) {
            auto& f = *init->fn;
            if (f.arity_val >= 0 && (int)args_saved.size() != f.arity_val)
                throw RuntimeError("Expected "+std::to_string(f.arity_val)+" args in _init, got "+std::to_string(args_saved.size()));
            if (f.is_native()) {
                f.call(std::move(args_saved));
                push(inst_val);
                return;
            }
            // Bytecode _init: push inst as receiver, push args, set up frame
            push(inst_val);                        // receiver at base-1
            for (auto& a : args_saved) push(a);   // args at base..base+argc-1
            size_t base = stack_.size() - args_saved.size();
            CallFrame fr;
            fr.fn    = init->fn;
            fr.chunk = init->fn->chunk;
            fr.ip    = 0;
            fr.base  = base;
            fr.self_val   = inst_val;
            fr.class_ptr  = callee.cls;
            fr.has_post_call   = true;
            fr.post_call_value = inst_val;
            frames_.push_back(std::move(fr));
            return;  // _init's RETURN will push inst_val via has_post_call
        }
        push(inst_val);
        return;
    }
    throw RuntimeError("Value is not callable");
}

void VM::do_method_call(const std::string& name, int argc) {
    // obj is at stack[sp - argc - 1]
    Value& obj = peek(argc);
    if (obj.is_inst()) {
        auto* m = obj.inst->klass->find_method(name);
        if (m && m->is_fn()) {
            auto& f = *m->fn;
            if (f.is_native()) {
                // native method: collect self+args
                Value self_copy = obj;
                std::vector<Value> args(stack_.end()-argc, stack_.end());
                stack_.erase(stack_.end()-argc-1, stack_.end()); // remove obj+args
                // Bound: call with self context
                // For native, just call the function
                push(f.call(std::move(args)));
                return;
            }
            // Bytecode method: set up frame
            size_t base = stack_.size() - argc;
            // obj is at base - 1
            CallFrame fr;
            fr.fn    = m->fn;
            fr.chunk = m->fn->chunk;
            fr.ip    = 0;
            fr.base  = base;
            fr.self_val  = obj;
            fr.class_ptr = obj.inst->klass;
            frames_.push_back(std::move(fr));
            return;
        }
        // Try do_get_attrib (handles lists, strings, maps, fibers, etc.)
        Value attr = do_get_attrib(obj, name);
        if (attr.is_fn() || attr.is_bound() || attr.is_class()) {
            stack_[stack_.size()-argc-1] = attr; // replace obj with attr
            do_call(attr, argc);
            return;
        }
        throw RuntimeError("Instance has no method '"+name+"'");
    }
    // Not an instance: get attr and call
    Value attr = do_get_attrib(obj, name);
    stack_[stack_.size()-argc-1] = attr;
    do_call(attr, argc);
}

void VM::do_super_call(const std::string& name, int argc) {
    if (frames_.empty()) throw RuntimeError("super outside method");
    auto& fr = frame();
    auto* parent = fr.class_ptr ? fr.class_ptr->parent.get() : nullptr;
    if (!parent) throw RuntimeError("No parent class for super");
    auto* m = parent->find_method(name);
    if (!m || !m->is_fn()) throw RuntimeError("Parent has no method '"+name+"'");

    auto& f = *m->fn;
    if (f.is_native()) {
        std::vector<Value> args(stack_.end()-argc, stack_.end());
        stack_.resize(stack_.size()-argc);
        push(f.call(std::move(args)));
        return;
    }
    // Push self as placeholder (we need it at base-1)
    // Stack currently has args at top. Insert self below them.
    Value self_copy = fr.self_val;
    // Shift args up by 1 to make room? No: base = sp - argc, and self is at base-1
    // But there's nothing to shift since args are already there.
    // The base will be sp-argc, and base-1 already has whatever was there (callee slot).
    // We just need to overwrite that slot with self for reference.
    size_t base = stack_.size() - argc;
    if (base > 0) stack_[base-1] = self_copy;

    CallFrame new_fr;
    new_fr.fn    = m->fn;
    new_fr.chunk = m->fn->chunk;
    new_fr.ip    = 0;
    new_fr.base  = base;
    new_fr.self_val  = self_copy;
    new_fr.class_ptr = fr.class_ptr ? fr.class_ptr->parent : nullptr;
    frames_.push_back(std::move(new_fr));
}

// ── Runtime operations ────────────────────────────────────────────────────────

static double require_num(const Value& v, const char* ctx) {
    if (!v.is_num()) throw RuntimeError(std::string("Expected number in ") + ctx);
    return v.n;
}
static long long to_int(double d) { return (long long)d; }

Value VM::do_binary(Opcode op, Value l, Value r) {
    // Equality
    if (op == Opcode::EQEQ) {
        if (l.is_inst()) {
            auto* m = l.inst->klass->find_method("==");
            if (m) return Value::make_bool(is_truthy(call_method(l, "==", {r})));
        }
        return Value::make_bool(l == r);
    }
    if (op == Opcode::NOTEQ) {
        if (l.is_inst()) {
            auto* m = l.inst->klass->find_method("==");
            if (m) return Value::make_bool(!is_truthy(call_method(l, "==", {r})));
        }
        return Value::make_bool(l != r);
    }
    // Instance operator overload
    if (l.is_inst()) {
        std::string on;
        switch (op) {
        case Opcode::LT:        on="<";  break; case Opcode::LTEQ:     on="<="; break;
        case Opcode::GT:        on=">";  break; case Opcode::GTEQ:     on=">="; break;
        case Opcode::ADD:       on="+";  break; case Opcode::SUBTRACT: on="-";  break;
        case Opcode::MULTIPLY:  on="*";  break; case Opcode::DIVIDE:   on="/";  break;
        case Opcode::MOD:       on="%";  break; case Opcode::EXPONENT: on="**"; break;
        case Opcode::BIT_LSHIFT:on="<<"; break; case Opcode::BIT_RSHIFT:on=">>";break;
        default: break;
        }
        if (!on.empty()) {
            auto* m = l.inst->klass->find_method(on);
            if (m) return call_method(l, on, {r});
        }
    }
    // Range
    if (op == Opcode::RANGE) {
        if (l.is_num() && r.is_num()) return Value::make_range(l.n, r.n);
        return Value::make_str(to_str(l) + to_str(r));
    }
    // Plus
    if (op == Opcode::ADD) {
        if (l.is_num() && r.is_num()) return Value::make_num(l.n + r.n);
        if (l.is_list() || r.is_list()) {
            auto ld = std::make_shared<ListData>();
            if (l.is_list()) for (auto& i : l.list->items) ld->items.push_back(i);
            if (r.is_list()) for (auto& i : r.list->items) ld->items.push_back(i);
            return Value::make_list(ld);
        }
        return Value::make_str(to_str(l) + to_str(r));
    }
    if (op == Opcode::SUBTRACT) return Value::make_num(require_num(l,"-") - require_num(r,"-"));
    if (op == Opcode::MULTIPLY) {
        if (l.is_num() && r.is_num()) return Value::make_num(l.n * r.n);
        if (l.is_str() && r.is_num()) { std::string res; for(int i=0;i<(int)r.n;++i) res+=l.s; return Value::make_str(res); }
        if (r.is_str() && l.is_num()) { std::string res; for(int i=0;i<(int)l.n;++i) res+=r.s; return Value::make_str(res); }
    }
    if (op == Opcode::DIVIDE)   return Value::make_num(require_num(l,"/") / require_num(r,"/"));
    if (op == Opcode::MOD)      return Value::make_num(std::fmod(require_num(l,"%"), require_num(r,"%")));
    if (op == Opcode::EXPONENT) return Value::make_num(std::pow(require_num(l,"**"), require_num(r,"**")));
    if (op == Opcode::LT)       return Value::make_bool(require_num(l,"<")  < require_num(r,"<"));
    if (op == Opcode::LTEQ)     return Value::make_bool(require_num(l,"<=") <= require_num(r,"<="));
    if (op == Opcode::GT)       return Value::make_bool(require_num(l,">")  > require_num(r,">"));
    if (op == Opcode::GTEQ)     return Value::make_bool(require_num(l,">=") >= require_num(r,">="));
    if (op == Opcode::BIT_OR)   return Value::make_num((double)(to_int(require_num(l,"|"))  | to_int(require_num(r,"|"))));
    if (op == Opcode::BIT_AND)  return Value::make_num((double)(to_int(require_num(l,"&"))  & to_int(require_num(r,"&"))));
    if (op == Opcode::BIT_XOR)  return Value::make_num((double)(to_int(require_num(l,"^"))  ^ to_int(require_num(r,"^"))));
    if (op == Opcode::BIT_LSHIFT) return Value::make_num((double)(to_int(require_num(l,"<<")) << (int)require_num(r,"<<")));
    if (op == Opcode::BIT_RSHIFT) return Value::make_num((double)(to_int(require_num(l,">>")) >> (int)require_num(r,">>")));
    throw RuntimeError("Unknown binary opcode");
}

Value VM::do_subscript(const Value& obj, const Value& idx) {
    if (obj.is_list()) {
        if (idx.is_range()) return do_slice(obj, idx.range.first, idx.range.second);
        int i = (int)require_num(idx,"list index");
        if (i < 0) i = (int)obj.list->items.size() + i;
        if (i < 0 || i >= (int)obj.list->items.size()) throw RuntimeError("List index out of range");
        return obj.list->items[i];
    }
    if (obj.is_str()) {
        if (idx.is_range()) return do_slice(obj, idx.range.first, idx.range.second);
        int i = (int)require_num(idx,"string index");
        if (i < 0) i = (int)obj.s.size() + i;
        if (i < 0 || i >= (int)obj.s.size()) throw RuntimeError("String index out of range");
        return Value::make_str(std::string(1, obj.s[i]));
    }
    if (obj.is_map()) {
        auto* v = obj.map->find(idx);
        if (!v) throw RuntimeError("Map key not found: " + to_str(idx));
        return *v;
    }
    if (obj.is_mod()) {
        auto it = obj.mod->attrs.find(to_str(idx));
        if (it == obj.mod->attrs.end()) throw RuntimeError("Module attr not found");
        return it->second;
    }
    throw RuntimeError("Cannot index this type");
}

void VM::do_subscript_assign(Value& obj, const Value& idx, Value val) {
    if (obj.is_list()) {
        int i = (int)require_num(idx,"list index");
        if (i < 0) i = (int)obj.list->items.size() + i;
        if (i < 0 || i >= (int)obj.list->items.size()) throw RuntimeError("List index out of range");
        obj.list->items[i] = std::move(val);
        return;
    }
    if (obj.is_map()) { obj.map->set(idx, std::move(val)); return; }
    throw RuntimeError("Cannot assign to index of this type");
}

Value VM::do_slice(const Value& obj, double a, double b) {
    if (obj.is_str()) {
        const std::string& s = obj.s; int n = (int)s.size();
        int ia = (int)a; if(ia<0)ia=n+ia; ia=std::max(0,std::min(ia,n>0?n-1:0));
        int ib = (int)b; if(ib<0)ib=n+ib;
        if (n==0) return Value::make_str("");
        std::string res;
        if (ia<=ib) { ib=std::min(ib,n-1); for(int i=ia;i<=ib;++i) res+=s[i]; }
        else        { ib=std::max(ib,0);   for(int i=ia;i>=ib;--i) res+=s[i]; }
        return Value::make_str(res);
    }
    if (obj.is_list()) {
        auto& items = obj.list->items; int n = (int)items.size();
        if (n==0) return Value::make_list();
        int ia = (int)a; if(ia<0)ia=n+ia; ia=std::max(0,std::min(ia,n-1));
        int ib = (int)b; if(ib<0)ib=n+ib;
        auto result = std::make_shared<ListData>();
        if (ia<=ib) { ib=std::min(ib,n-1); for(int i=ia;i<=ib;++i) result->items.push_back(items[i]); }
        else        { ib=std::max(ib,0);   for(int i=ia;i>=ib;--i) result->items.push_back(items[i]); }
        return Value::make_list(result);
    }
    throw RuntimeError("slice: not sliceable");
}

Value VM::get_attr(const Value& obj, const std::string& name) {
    if (obj.is_str()) {
        if (name=="length") return Value::make_num((double)obj.s.size());
        auto sv = obj;
        if (name=="lower")  return make_native("lower",0,[sv](auto){std::string r=sv.s;for(auto&c:r)c=tolower(c);return Value::make_str(r);});
        if (name=="upper")  return make_native("upper",0,[sv](auto){std::string r=sv.s;for(auto&c:r)c=toupper(c);return Value::make_str(r);});
        if (name=="strip")  return make_native("strip",0,[sv](auto){std::string s=sv.s;size_t a=s.find_first_not_of(" \t\n\r");if(a==std::string::npos)return Value::make_str("");size_t b=s.find_last_not_of(" \t\n\r");return Value::make_str(s.substr(a,b-a+1));});
        if (name=="find")   return make_native("find",1,[sv](std::vector<Value> args){if(!args[0].is_str())throw RuntimeError("find: expected string");auto p=sv.s.find(args[0].s);return Value::make_num(p==std::string::npos?-1:(double)p);});
        if (name=="replace")return make_native("replace",-1,[sv](std::vector<Value> args){if(args.size()<2)throw RuntimeError("replace: need 2 args");std::string s=sv.s,from=args[0].s,to=args[1].s;int max_c=args.size()>2?(int)args[2].n:INT_MAX;int cnt=0;size_t pos=0;std::string result;while((pos=s.find(from,pos))!=std::string::npos&&cnt<max_c){result+=s.substr(0,pos);result+=to;s=s.substr(pos+from.size());pos=0;++cnt;}result+=s;return Value::make_str(result);});
        if (name=="split")  return make_native("split",1,[sv](std::vector<Value> args){std::string s=sv.s,delim=args[0].s;auto result=std::make_shared<ListData>();if(delim.empty()){for(char c:s)result->items.push_back(Value::make_str(std::string(1,c)));}else{size_t pos=0,next;while((next=s.find(delim,pos))!=std::string::npos){result->items.push_back(Value::make_str(s.substr(pos,next-pos)));pos=next+delim.size();}result->items.push_back(Value::make_str(s.substr(pos)));}return Value::make_list(result);});
        if (name=="startswith")return make_native("startswith",1,[sv](std::vector<Value> args){auto check=[&sv](const std::string&p){return sv.s.substr(0,p.size())==p;};if(args[0].is_str())return Value::make_bool(check(args[0].s));if(args[0].is_list()){for(auto&i:args[0].list->items)if(i.is_str()&&check(i.s))return Value::make_bool(true);return Value::make_bool(false);}return Value::make_bool(false);});
        if (name=="endswith")  return make_native("endswith",1,[sv](std::vector<Value> args){auto check=[&sv](const std::string&s){return sv.s.size()>=s.size()&&sv.s.substr(sv.s.size()-s.size())==s;};if(args[0].is_str())return Value::make_bool(check(args[0].s));if(args[0].is_list()){for(auto&i:args[0].list->items)if(i.is_str()&&check(i.s))return Value::make_bool(true);return Value::make_bool(false);}return Value::make_bool(false);});
        throw RuntimeError("String has no attribute '"+name+"'");
    }
    if (obj.is_list()) {
        if (name=="length") return Value::make_num((double)obj.list->items.size());
        auto lv = obj;
        if (name=="append") return make_native("append",1,[lv](std::vector<Value> args){lv.list->items.push_back(args[0]);return lv;});
        if (name=="find")   return make_native("find",1,[lv](std::vector<Value> args){auto&items=lv.list->items;for(int i=0;i<(int)items.size();++i)if(items[i]==args[0])return Value::make_num(i);return Value::make_num(-1);});
        if (name=="pop")    return make_native("pop",-1,[lv](std::vector<Value> args){auto&items=lv.list->items;if(items.empty())throw RuntimeError("pop: empty list");int idx=args.empty()?(int)items.size()-1:(int)args[0].n;if(idx<0)idx=(int)items.size()+idx;if(idx<0||idx>=(int)items.size())throw RuntimeError("pop: index out of range");Value v=items[idx];items.erase(items.begin()+idx);return v;});
        if (name=="insert") return make_native("insert",2,[lv](std::vector<Value> args){auto&items=lv.list->items;int idx=(int)args[0].n;if(idx<0)idx=(int)items.size()+idx;idx=std::max(0,std::min(idx,(int)items.size()));items.insert(items.begin()+idx,args[1]);return Value::make_null();});
        if (name=="sort")   return make_native("sort",-1,[this,lv](std::vector<Value> args) mutable {auto&items=lv.list->items;if(!args.empty()&&args[0].is_callable()){std::sort(items.begin(),items.end(),[this,&args](const Value&a,const Value&b){Value r=call_value(args[0],{a,b});return is_truthy(r);});}else{std::sort(items.begin(),items.end(),[](const Value&a,const Value&b){return a<b;});}return lv;});
        if (name=="as_list") return lv;
        if (name=="keys")   { auto md=std::make_shared<ListData>(); for(auto&i:lv.list->items) md->items.push_back(i); return Value::make_list(md); }
        throw RuntimeError("List has no attribute '"+name+"'");
    }
    if (obj.is_map()) {
        auto mv = obj;
        if (name=="get")    return make_native("get",-1,[mv](std::vector<Value> args){if(args.empty())throw RuntimeError("get: need key");auto*v=mv.map->find(args[0]);if(!v)return args.size()>1?args[1]:Value::make_null();return *v;});
        if (name=="has")    return make_native("has",1,[mv](std::vector<Value> args){return Value::make_bool(mv.map->has(args[0]));});
        if (name=="keys")   return make_native("keys",0,[mv](auto){auto ld=std::make_shared<ListData>();for(auto&[k,v]:mv.map->pairs)ld->items.push_back(k);return Value::make_list(ld);});
        if (name=="values") return make_native("values",0,[mv](auto){auto ld=std::make_shared<ListData>();for(auto&[k,v]:mv.map->pairs)ld->items.push_back(v);return Value::make_list(ld);});
        throw RuntimeError("Map has no attribute '"+name+"'");
    }
    if (obj.is_range()) {
        if (name=="first") return Value::make_num(obj.range.first);
        if (name=="last")  return Value::make_num(obj.range.second);
        if (name=="as_list") {
            auto ld = std::make_shared<ListData>();
            double a=obj.range.first, b=obj.range.second;
            if (a<=b) for(double i=a;i<b;++i) ld->items.push_back(Value::make_num(i));
            else      for(double i=a;i>b;--i) ld->items.push_back(Value::make_num(i));
            return Value::make_list(ld);
        }
        throw RuntimeError("Range has no attribute '"+name+"'");
    }
    if (obj.is_num()) {
        if (name=="times") {
            auto nv = obj;
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

Value VM::do_get_attrib(const Value& obj, const std::string& name) {
    if (obj.is_inst()) {
        auto it = obj.inst->attrs.find(name);
        if (it != obj.inst->attrs.end()) return it->second;
        auto* m = obj.inst->klass->find_method(name);
        if (m) {
            auto bm = std::make_shared<BoundMethodData>(); bm->self_val=obj; bm->fn=m->fn;
            return Value::make_bound(bm);
        }
        throw RuntimeError("Instance has no attribute '"+name+"'");
    }
    if (obj.is_class()) {
        auto it = obj.cls->class_attrs.find(name);
        if (it != obj.cls->class_attrs.end()) return it->second;
        auto* m = obj.cls->find_method(name);
        if (m) return *m;
        if (name=="_docs") return Value::make_str(obj.cls->docs);
        if (name=="parent") return obj.cls->parent ? Value::make_class(obj.cls->parent) : Value::make_null();
        throw RuntimeError("Class '"+obj.cls->name+"' has no attribute '"+name+"'");
    }
    if (obj.is_mod()) {
        auto it = obj.mod->attrs.find(name);
        if (it != obj.mod->attrs.end()) return it->second;
        throw RuntimeError("Module has no attribute '"+name+"'");
    }
    if (obj.is_fn()) {
        if (name=="arity") return Value::make_num(obj.fn->arity_val);
        if (name=="name")  return Value::make_str(obj.fn->name);
        if (name=="_docs") return Value::make_str(obj.fn->docs);
        if (name=="bind") {
            auto fn_val = obj;
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
        auto fib = obj;
        if (name=="run"||name=="resume") {
            return make_native(name,-1,[this,fib](std::vector<Value> args)->Value{
                return call_value(fib, std::move(args));
            });
        }
        throw RuntimeError("Fiber has no attribute '"+name+"'");
    }
    return get_attr(obj, name);
}

void VM::do_set_attrib(Value& obj, const std::string& name, Value val) {
    if (obj.is_inst()) { obj.inst->attrs[name] = std::move(val); return; }
    if (obj.is_class()) { obj.cls->class_attrs[name] = std::move(val); return; }
    if (obj.is_mod()) { obj.mod->attrs[name] = std::move(val); return; }
    throw RuntimeError("Cannot set attribute on this type");
}

Value VM::call_method(Value self, const std::string& name, std::vector<Value> args) {
    if (self.is_inst()) {
        auto* m = self.inst->klass->find_method(name);
        if (!m || !m->is_fn()) throw RuntimeError("No method '"+name+"' on "+self.inst->klass->name);
        auto& f = *m->fn;
        if (f.is_native()) return f.call(std::move(args));
        // Push self + args on stack, set up frame
        push(self); // callee/receiver slot
        for (auto& a : args) push(a);
        size_t base = stack_.size() - args.size();
        CallFrame fr;
        fr.fn = m->fn; fr.chunk = m->fn->chunk; fr.ip = 0;
        fr.base = base; fr.self_val = self; fr.class_ptr = self.inst->klass;
        frames_.push_back(std::move(fr));
        return run_inner();
    }
    Value attr = get_attr(self, name);
    return call_value(attr, std::move(args));
}

// Run the inner execution loop until current top frame returns
// Used by call_method for recursive calls
Value VM::run_inner() {
    size_t target_depth = frames_.size() - 1;
    Value ret = Value::make_null();
    run_loop(target_depth, ret);
    return ret;
}

// ── call_value (for native/bound/class invocation from builtins) ──────────────

Value VM::call_value(const Value& callee, std::vector<Value> args) {
    if (callee.is_fn()) {
        auto& f = *callee.fn;
        if (f.arity_val >= 0 && (int)args.size() != f.arity_val)
            throw RuntimeError("Expected "+std::to_string(f.arity_val)+" args, got "+std::to_string(args.size()));
        if (f.is_native()) return f.call(std::move(args));
        // Push a dummy receiver + args
        push(callee);
        for (auto& a : args) push(a);
        size_t base = stack_.size() - args.size();
        CallFrame fr;
        fr.fn = callee.fn; fr.chunk = callee.fn->chunk; fr.ip = 0;
        fr.base = base; fr.self_val = Value::make_null(); fr.class_ptr = nullptr;
        frames_.push_back(std::move(fr));
        return run_inner();
    }
    if (callee.is_bound()) {
        auto& bm = *callee.bound;
        if (bm.fn->arity_val >= 0 && (int)args.size() != bm.fn->arity_val)
            throw RuntimeError("Expected "+std::to_string(bm.fn->arity_val)+" args, got "+std::to_string(args.size()));
        if (bm.fn->is_native()) return bm.fn->call(std::move(args));
        push(callee);
        for (auto& a : args) push(a);
        size_t base = stack_.size() - args.size();
        CallFrame fr;
        fr.fn = bm.fn; fr.chunk = bm.fn->chunk; fr.ip = 0;
        fr.base = base; fr.self_val = bm.self_val;
        fr.class_ptr = bm.self_val.is_inst() ? bm.self_val.inst->klass : nullptr;
        frames_.push_back(std::move(fr));
        return run_inner();
    }
    if (callee.is_class()) {
        auto inst = std::make_shared<InstanceData>();
        inst->klass = callee.cls;
        Value inst_val = Value::make_inst(inst);
        auto* init = callee.cls->find_method("_init");
        if (init && init->is_fn()) {
            auto& f = *init->fn;
            if (f.arity_val >= 0 && (int)args.size() != f.arity_val)
                throw RuntimeError("Expected "+std::to_string(f.arity_val)+" args in _init");
            if (f.is_native()) {
                f.call(std::move(args));
            } else {
                push(inst_val); // receiver slot
                for (auto& a : args) push(a);
                size_t base = stack_.size() - args.size();
                CallFrame fr;
                fr.fn = init->fn; fr.chunk = init->fn->chunk; fr.ip = 0;
                fr.base = base; fr.self_val = inst_val; fr.class_ptr = callee.cls;
                frames_.push_back(std::move(fr));
                run_inner();
            }
        }
        return inst_val;
    }
    if (callee.is_fiber()) {
        auto& fib = *callee.fiber;
        if (fib.state == FiberState::DONE) throw RuntimeError("Fiber is done");
        auto impl_ptr = std::static_pointer_cast<FiberImpl>(fib.impl);
        if (fib.state == FiberState::CREATED) {
            auto new_impl = std::make_shared<FiberImpl>();
            fib.impl = new_impl;
            auto fib_ptr = callee.fiber;
            auto fn_val  = fib.fn_val;
            std::vector<Value> run_args = std::move(args);

            new_impl->thread = std::thread([this, fib_ptr, fn_val, run_args=std::move(run_args), impl=new_impl.get()]() mutable {
                auto prev_fiber = current_fiber_;
                current_fiber_ = impl;
                Value result;
                try { result = call_value(fn_val, run_args); }
                catch (...) {
                    current_fiber_ = prev_fiber;
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
                std::lock_guard<std::mutex> lk(impl->mu);
                impl->recv_val = result;
                fib_ptr->state = FiberState::DONE;
                impl->state = FiberState::DONE;
                impl->caller_ready = true;
                impl->caller_cv.notify_one();
            });
            fib.state = FiberState::RUNNING;
            std::unique_lock<std::mutex> lk(new_impl->mu);
            new_impl->caller_cv.wait(lk, [&]{ return new_impl->caller_ready; });
            new_impl->caller_ready = false;
            fib.state = new_impl->state;
            if (new_impl->exc_ptr) std::rethrow_exception(new_impl->exc_ptr);
            return new_impl->recv_val;
        } else {
            if (!impl_ptr) throw RuntimeError("Fiber not initialized");
            Value resume_val = args.empty() ? Value::make_null() : args[0];
            {
                std::lock_guard<std::mutex> lk(impl_ptr->mu);
                impl_ptr->send_val = resume_val;
                fib.state = FiberState::RUNNING;
                impl_ptr->state = FiberState::RUNNING;
                impl_ptr->fiber_ready = true;
            }
            impl_ptr->fiber_cv.notify_one();
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

Value VM::import_module(const std::string& path) {
    auto it = modules_.find(path);
    if (it != modules_.end()) return it->second;
    static const std::string builtins[] = {"lang","io","time","path","math"};
    for (auto& b : builtins) if (path==b) {
        auto mod = std::make_shared<ModuleData>(); mod->name = path;
        return Value::make_mod(mod);
    }
    std::string rel = path;
    std::string up_prefix;
    while (!rel.empty() && rel[0]=='^') { up_prefix += "../"; rel = rel.substr(1); }
    for (char& c : rel) if (c=='.') c='/';
    rel += ".pk";

    std::vector<std::string> search_paths;
    if (!base_dir_.empty()) {
        search_paths.push_back(base_dir_ + "/" + up_prefix + rel);
        std::string init_path = base_dir_ + "/" + up_prefix + rel.substr(0,rel.size()-3) + "/_init.pk";
        search_paths.push_back(init_path);
    }
    search_paths.push_back(up_prefix + rel);
    std::string full_path;
    for (auto& p : search_paths) if (fs::exists(p)) { full_path = p; break; }
    if (full_path.empty()) throw RuntimeError("Module not found: "+path);

    std::string cache_key;
    try { cache_key = fs::canonical(full_path).string(); }
    catch (...) { cache_key = full_path; }

    auto it2 = modules_.find(cache_key);
    if (it2 != modules_.end()) { modules_[path] = it2->second; return it2->second; }

    auto mod = std::make_shared<ModuleData>(); mod->name = path; mod->path = full_path;
    Value mod_val = Value::make_mod(mod);
    modules_[cache_key] = mod_val;
    modules_[path] = mod_val;
    exec_module(full_path, *mod);
    return mod_val;
}

void VM::exec_module(const std::string& path, ModuleData& mod) {
    std::ifstream f(path);
    if (!f) throw RuntimeError("Cannot open module: "+path);
    std::ostringstream ss; ss << f.rdbuf();
    std::string src = ss.str();
    std::string mod_base = fs::path(path).parent_path().string();
    if (mod_base.empty()) mod_base = ".";

    Lexer lex(src);
    auto tokens = lex.tokenize();
    Parser parser(std::move(tokens));
    auto stmts = parser.parse();

    Compiler comp;
    auto chunk = comp.compile_script(stmts);

    // Run in a sub-VM sharing globals and modules
    // but with mod_base as the base_dir
    std::ostringstream mod_out;
    std::unordered_map<std::string, Value> mod_globals = globals_; // copy globals (includes builtins)
    std::string old_base = base_dir_;

    VM sub_vm(mod_globals, mod_out, mod_base, modules_);
    sub_vm.run(chunk);
    out_ << mod_out.str();

    // Export non-builtin names
    static const std::set<std::string> skip_set = {
        "print","assert","str","type","hex","Number","clock","dir",
        "list_append","list_join","min","max","Fiber",
        "String","List","Map","Range","lang","io","time","path","math",
        "__method__"
    };
    for (auto& [name, val] : mod_globals) {
        // Export new names that weren't in the original globals
        if (!globals_.count(name) || !skip_set.count(name))
            if (!skip_set.count(name)) mod.attrs[name] = val;
    }
}

// ── Main execution loop ───────────────────────────────────────────────────────

// Run until frames drops to target_depth (used for recursive calls)
void VM::run_loop(size_t target_depth, Value& ret_val) {
    while (frames_.size() > target_depth) {
        auto& fr = frames_.back();
        Opcode op = (Opcode)read_byte();

        switch (op) {
        case Opcode::PUSH_CONSTANT: {
            uint16_t idx = read_u16();
            push(fr.chunk->constants[idx]);
            break;
        }
        case Opcode::PUSH_NULL:  push(Value::make_null());   break;
        case Opcode::PUSH_0:     push(Value::make_num(0));   break;
        case Opcode::PUSH_TRUE:  push(Value::make_bool(true));  break;
        case Opcode::PUSH_FALSE: push(Value::make_bool(false)); break;
        case Opcode::POP:   pop(); break;
        case Opcode::DUP:   push(peek(0)); break;
        case Opcode::SWAP: { Value a=pop(), b=pop(); push(a); push(b); break; }

        case Opcode::PUSH_LOCAL_0: case Opcode::PUSH_LOCAL_1:
        case Opcode::PUSH_LOCAL_2: case Opcode::PUSH_LOCAL_3:
        case Opcode::PUSH_LOCAL_4: case Opcode::PUSH_LOCAL_5:
        case Opcode::PUSH_LOCAL_6: case Opcode::PUSH_LOCAL_7:
        case Opcode::PUSH_LOCAL_8: {
            int slot = (int)op - (int)Opcode::PUSH_LOCAL_0;
            push(stack_[fr.base + slot]);
            break;
        }
        case Opcode::PUSH_LOCAL_N: {
            uint8_t slot = read_byte();
            push(stack_[fr.base + slot]);
            break;
        }
        case Opcode::STORE_LOCAL_0: case Opcode::STORE_LOCAL_1:
        case Opcode::STORE_LOCAL_2: case Opcode::STORE_LOCAL_3:
        case Opcode::STORE_LOCAL_4: case Opcode::STORE_LOCAL_5:
        case Opcode::STORE_LOCAL_6: case Opcode::STORE_LOCAL_7:
        case Opcode::STORE_LOCAL_8: {
            int slot = (int)op - (int)Opcode::STORE_LOCAL_0;
            stack_[fr.base + slot] = peek(0);
            break;
        }
        case Opcode::STORE_LOCAL_N: {
            uint8_t slot = read_byte();
            stack_[fr.base + slot] = peek(0);
            break;
        }
        case Opcode::PUSH_GLOBAL: {
            uint16_t idx = read_u16();
            auto& name = fr.chunk->constants[idx].s;
            auto it = globals_.find(name);
            push(it != globals_.end() ? it->second : Value::make_null());
            break;
        }
        case Opcode::STORE_GLOBAL: {
            uint16_t idx = read_u16();
            auto& name = fr.chunk->constants[idx].s;
            globals_[name] = peek(0);
            break;
        }
        case Opcode::PUSH_UPVALUE: {
            uint8_t idx = read_byte();
            push(fr.fn->upvalues[idx]->deref());
            break;
        }
        case Opcode::STORE_UPVALUE: {
            uint8_t idx = read_byte();
            fr.fn->upvalues[idx]->deref() = peek(0);
            break;
        }
        case Opcode::PUSH_SELF: push(fr.self_val); break;

        case Opcode::PUSH_LIST: {
            uint16_t count = read_u16();
            auto ld = std::make_shared<ListData>();
            ld->items.resize(count);
            for (int i = count-1; i >= 0; --i) ld->items[i] = pop();
            push(Value::make_list(ld));
            break;
        }
        case Opcode::PUSH_MAP:    push(Value::make_map()); break;
        case Opcode::LIST_APPEND: { Value v=pop(); peek(0).list->items.push_back(v); break; }
        case Opcode::MAP_INSERT:  {
            Value val=pop(), key=pop();
            peek(0).map->set(key, val);
            break;
        }

        case Opcode::PUSH_CLOSURE: {
            uint16_t idx = read_u16();
            // constants[idx] is a FuncData with chunk but no upvalues yet
            auto template_fn = fr.chunk->constants[idx].fn;
            // Create a new FuncData sharing the same chunk
            auto new_fn = std::make_shared<FuncData>();
            new_fn->name      = template_fn->name;
            new_fn->arity_val = template_fn->arity_val;
            new_fn->chunk     = template_fn->chunk;
            new_fn->docs      = template_fn->docs;
            // Read upvalue descriptors from bytecode
            int uv_count = template_fn->chunk->upvalue_count;
            new_fn->upvalues.resize(uv_count);
            for (int i = 0; i < uv_count; ++i) {
                uint8_t is_local = read_byte();
                uint8_t uv_idx   = read_byte();
                if (is_local) {
                    new_fn->upvalues[i] = capture_upvalue(fr.base + uv_idx);
                } else {
                    new_fn->upvalues[i] = fr.fn->upvalues[uv_idx];
                }
            }
            push(Value::make_fn(new_fn));
            break;
        }

        case Opcode::CREATE_CLASS: {
            uint16_t name_idx = read_u16();
            std::string name = fr.chunk->constants[name_idx].s;
            Value parent = pop();
            auto cls = std::make_shared<ClassData>();
            cls->name = name;
            if (parent.is_class()) cls->parent = parent.cls;
            push(Value::make_class(cls));
            break;
        }
        case Opcode::BIND_METHOD: {
            uint16_t name_idx = read_u16();
            std::string mname = fr.chunk->constants[name_idx].s;
            Value closure = pop(); // method closure is TOS
            // class is now TOS
            peek(0).cls->methods[mname] = closure;
            break;
        }

        case Opcode::CLOSE_UPVALUE: {
            close_upvalues(stack_.size() - 1);
            pop();
            break;
        }

        case Opcode::IMPORT: {
            uint16_t idx = read_u16();
            std::string path = fr.chunk->constants[idx].s;
            push(import_module(path));
            break;
        }

        case Opcode::CALL: {
            uint8_t argc = read_byte();
            Value callee = stack_[stack_.size()-argc-1];
            do_call(callee, argc, false);
            break;
        }
        case Opcode::TAIL_CALL: {
            uint8_t argc = read_byte();
            Value callee = stack_[stack_.size()-argc-1];
            do_call(callee, argc, true);
            break;
        }
        case Opcode::METHOD_CALL: {
            uint16_t name_idx = read_u16();
            uint8_t  argc     = read_byte();
            std::string name = fr.chunk->constants[name_idx].s;
            do_method_call(name, argc);
            break;
        }
        case Opcode::SUPER_CALL: {
            uint16_t name_idx = read_u16();
            uint8_t  argc     = read_byte();
            std::string name = fr.chunk->constants[name_idx].s;
            do_super_call(name, argc);
            break;
        }

        case Opcode::RETURN: {
            Value ret = pop();
            size_t base = fr.base;
            bool   hpc  = fr.has_post_call;
            Value  pv   = fr.post_call_value;
            close_upvalues(base);
            frames_.pop_back();
            // Remove callee/receiver + all locals
            if (base > 0) stack_.resize(base - 1);
            else          stack_.resize(0);
            push(hpc ? pv : ret);
            if (frames_.size() == target_depth) {
                ret_val = hpc ? pv : ret;
                return;
            }
            break;
        }

        case Opcode::ITER_TEST: {
            // Validate TOS-2 (container) is iterable
            Value& container = stack_[stack_.size()-3];
            if (!container.is_list() && !container.is_str() && !container.is_range() && !container.is_map())
                throw RuntimeError("Value is not iterable");
            break;
        }
        case Opcode::ITER: {
            uint8_t hi = read_byte(), lo = read_byte();
            uint16_t jump = (uint16_t)((hi << 8) | lo);
            // Stack: [..., container, counter, item]
            Value& container = stack_[stack_.size()-3];
            Value& counter   = stack_[stack_.size()-2];
            Value& item      = stack_[stack_.size()-1];
            int idx = (int)counter.n;
            bool done = false;
            if (container.is_list()) {
                if (idx >= (int)container.list->items.size()) done = true;
                else { item = container.list->items[idx]; counter.n++; }
            } else if (container.is_str()) {
                if (idx >= (int)container.s.size()) done = true;
                else { item = Value::make_str(std::string(1,container.s[idx])); counter.n++; }
            } else if (container.is_range()) {
                double a = container.range.first, b = container.range.second;
                double cur;
                if (a <= b) cur = a + idx;
                else        cur = a - idx;
                bool in_range = (a <= b) ? (cur < b) : (cur > b);
                if (!in_range) done = true;
                else { item = Value::make_num(cur); counter.n++; }
            } else if (container.is_map()) {
                if (idx >= (int)container.map->pairs.size()) done = true;
                else { item = container.map->pairs[idx].first; counter.n++; }
            } else {
                done = true;
            }
            if (done) fr.ip += jump;
            break;
        }

        case Opcode::YIELD: {
            Value yield_val = pop();
            if (!current_fiber_) throw RuntimeError("yield outside fiber");
            auto* impl = current_fiber_;
            {
                std::lock_guard<std::mutex> lk(impl->mu);
                impl->recv_val = yield_val;
                impl->state = FiberState::SUSPENDED;
                // Update the FiberData state too
                impl->caller_ready = true;
            }
            impl->caller_cv.notify_one();
            // Wait for resume
            {
                std::unique_lock<std::mutex> lk(impl->mu);
                impl->fiber_cv.wait(lk, [&]{ return impl->fiber_ready || impl->terminated; });
                impl->fiber_ready = false;
            }
            if (impl->terminated) throw RuntimeError("fiber terminated");
            push(impl->send_val);
            break;
        }

        case Opcode::JUMP: {
            uint8_t hi = read_byte(), lo = read_byte();
            fr.ip += (uint16_t)((hi << 8) | lo);
            break;
        }
        case Opcode::LOOP: {
            uint8_t hi = read_byte(), lo = read_byte();
            fr.ip -= (uint16_t)((hi << 8) | lo);
            break;
        }
        case Opcode::JUMP_IF: {
            uint8_t hi = read_byte(), lo = read_byte();
            if (is_truthy(pop())) fr.ip += (uint16_t)((hi << 8) | lo);
            break;
        }
        case Opcode::JUMP_IF_NOT: {
            uint8_t hi = read_byte(), lo = read_byte();
            if (!is_truthy(pop())) fr.ip += (uint16_t)((hi << 8) | lo);
            break;
        }
        case Opcode::OR: {
            uint8_t hi = read_byte(), lo = read_byte();
            if (is_truthy(peek(0))) { fr.ip += (uint16_t)((hi<<8)|lo); }
            else pop();
            break;
        }
        case Opcode::AND: {
            uint8_t hi = read_byte(), lo = read_byte();
            if (!is_truthy(peek(0))) { fr.ip += (uint16_t)((hi<<8)|lo); }
            else pop();
            break;
        }

        case Opcode::GET_ATTRIB: {
            uint16_t idx = read_u16();
            std::string name = fr.chunk->constants[idx].s;
            Value obj = pop();
            push(do_get_attrib(obj, name));
            break;
        }
        case Opcode::GET_ATTRIB_KEEP: {
            uint16_t idx = read_u16();
            std::string name = fr.chunk->constants[idx].s;
            Value attr = do_get_attrib(peek(0), name);
            push(attr);
            break;
        }
        case Opcode::SET_ATTRIB: {
            uint16_t idx = read_u16();
            std::string name = fr.chunk->constants[idx].s;
            Value val = pop();
            do_set_attrib(peek(0), name, val);
            // leave the value on TOS by pushing it back? No: the object is on TOS.
            // Actually SET_ATTRIB should leave the assigned value on TOS for assignment expressions
            pop(); // remove obj
            push(val); // push value (assignment result)
            break;
        }
        case Opcode::GET_SUBSCRIPT: {
            Value idx = pop(), obj = pop();
            push(do_subscript(obj, idx));
            break;
        }
        case Opcode::GET_SUBSCRIPT_KEEP: {
            // Stack: [..., obj, idx] → [..., obj, idx, value]
            Value val = do_subscript(peek(1), peek(0));
            push(val);
            break;
        }
        case Opcode::SET_SUBSCRIPT: {
            // Stack: [..., obj, idx, value] → [..., value]
            Value val = pop(), idx = pop();
            Value& obj_ref = peek(0);
            do_subscript_assign(obj_ref, idx, val);
            pop(); // remove obj
            push(val); // push assigned value
            break;
        }

        case Opcode::POSITIVE: { Value v=pop(); push(Value::make_num(+v.n)); break; }
        case Opcode::NEGATIVE: {
            Value v = pop();
            if (!v.is_num()) throw RuntimeError("Unary '-' on non-number");
            push(Value::make_num(-v.n));
            break;
        }
        case Opcode::NOT: { Value v=pop(); push(Value::make_bool(!is_truthy(v))); break; }
        case Opcode::BIT_NOT: {
            Value v = pop();
            push(Value::make_num((double)(~(long long)v.n)));
            break;
        }

        case Opcode::ADD: case Opcode::SUBTRACT: case Opcode::MULTIPLY:
        case Opcode::DIVIDE: case Opcode::EXPONENT: case Opcode::MOD:
        case Opcode::BIT_AND: case Opcode::BIT_OR: case Opcode::BIT_XOR:
        case Opcode::BIT_LSHIFT: case Opcode::BIT_RSHIFT:
        case Opcode::EQEQ: case Opcode::NOTEQ:
        case Opcode::LT: case Opcode::LTEQ: case Opcode::GT: case Opcode::GTEQ:
        case Opcode::RANGE: {
            Value r = pop(), l = pop();
            push(do_binary(op, std::move(l), std::move(r)));
            break;
        }

        case Opcode::EXTEND_INPLACE: {
            // Like ADD but mutates list LHS in-place (shared references remain valid)
            Value r = pop();
            Value l = pop();
            if (l.is_list() && r.is_list()) {
                // snapshot RHS to avoid aliasing when l and r share the same ListData
                auto r_items = r.list->items;
                for (auto& item : r_items) l.list->items.push_back(item);
                push(l);
            } else {
                push(do_binary(Opcode::ADD, std::move(l), std::move(r)));
            }
            break;
        }

        case Opcode::IN: {
            Value hay = pop(), needle = pop();
            bool found = false;
            if (hay.is_list()) {
                for (auto& item : hay.list->items) if (item == needle) { found=true; break; }
            } else if (hay.is_str() && needle.is_str()) {
                found = hay.s.find(needle.s) != std::string::npos;
            } else if (hay.is_map()) {
                found = hay.map->has(needle);
            }
            push(Value::make_bool(found));
            break;
        }
        case Opcode::NOT_IN: {
            Value hay = pop(), needle = pop();
            bool found = false;
            if (hay.is_list()) {
                for (auto& item : hay.list->items) if (item == needle) { found=true; break; }
            } else if (hay.is_str() && needle.is_str()) {
                found = hay.s.find(needle.s) != std::string::npos;
            } else if (hay.is_map()) {
                found = hay.map->has(needle);
            }
            push(Value::make_bool(!found));
            break;
        }
        case Opcode::IS: {
            Value cls = pop(), obj = pop();
            bool result = false;
            if (obj.is_inst() && cls.is_class()) {
                auto* klass = obj.inst->klass.get();
                while (klass) {
                    if (klass == cls.cls.get()) { result=true; break; }
                    klass = klass->parent.get();
                }
            }
            push(Value::make_bool(result));
            break;
        }

        case Opcode::STRING_CONCAT: {
            uint8_t count = read_byte();
            std::string result;
            // items are at stack end: [part0, part1, ..., partN-1]
            size_t start = stack_.size() - count;
            for (size_t i = start; i < stack_.size(); ++i)
                result += to_str(stack_[i]);
            stack_.resize(start);
            push(Value::make_str(result));
            break;
        }

        case Opcode::END:
            ret_val = Value::make_null();
            return;

        default:
            throw RuntimeError("Unknown opcode: " + std::to_string((int)op));
        }
    }
}

// ── Built-in functions ────────────────────────────────────────────────────────

void VM::register_builtins() {
    auto& g = globals_;

    g["print"] = make_native("print", -1, [this](std::vector<Value> args) {
        std::string sep;
        for (auto& a : args) { out_ << sep << to_str(a); sep = " "; }
        out_ << "\n";
        return Value::make_null();
    });

    g["assert"] = make_native("assert", -1, [this](std::vector<Value> args) {
        if (args.empty()) throw RuntimeError("assert requires at least one argument");
        if (!is_truthy(args[0])) {
            std::string msg = args.size() > 1 ? to_str(args[1]) : "Assertion failed.";
            throw AssertError(msg);
        }
        return Value::make_null();
    });

    g["str"] = make_native("str", 1, [this](std::vector<Value> args) {
        return Value::make_str(to_str(args[0]));
    });

    g["type"] = make_native("type", 1, [](std::vector<Value> args) {
        static const char* names[] = {"Null","Bool","Number","String","List","Map","Range",
                                       "Function","BoundMethod","Class","Instance","Fiber","Module"};
        return Value::make_str(names[(int)args[0].type]);
    });

    g["hex"] = make_native("hex", 1, [](std::vector<Value> args) {
        long long n = (long long)args[0].n;
        bool neg = n < 0;
        unsigned long long un = neg ? (unsigned long long)(-n) : (unsigned long long)n;
        char buf[64];
        snprintf(buf, sizeof(buf), "%s0x%llx", neg ? "-" : "", un);
        return Value::make_str(buf);
    });

    g["Number"] = make_native("Number", 1, [](std::vector<Value> args) {
        if (args[0].is_num()) return args[0];
        if (args[0].is_str()) {
            std::string s = args[0].s;
            bool neg = false;
            if (!s.empty() && s[0]=='-') { neg=true; s=s.substr(1); }
            while (!s.empty() && isspace(s[0])) s=s.substr(1);
            double v = 0;
            bool ok = false;
            try {
                if (s.size()>2 && s[0]=='0' && (s[1]=='b'||s[1]=='B')) {
                    v=0; for(char c:s.substr(2)){v*=2;v+=(c-'0');} ok=true;
                } else if (s.size()>2 && s[0]=='0' && (s[1]=='x'||s[1]=='X')) {
                    v=(double)std::stoull(s,nullptr,16); ok=true;
                } else { size_t idx; v=std::stod(s,&idx); ok=(idx>0); }
            } catch(...) {}
            if (ok) return Value::make_num(neg ? -v : v);
        }
        return Value::make_null();
    });

    g["clock"] = make_native("clock", 0, [](std::vector<Value>) {
        return Value::make_num((double)std::clock() / CLOCKS_PER_SEC);
    });

    g["dir"] = make_native("dir", 1, [this](std::vector<Value> args) {
        auto list = std::make_shared<ListData>();
        auto add = [&](const std::string& s){ list->items.push_back(Value::make_str(s)); };
        auto& v = args[0];
        if (v.is_inst()) {
            std::set<std::string> seen;
            for (auto& [k,_]:v.inst->attrs){if(!seen.count(k)){add(k);seen.insert(k);}}
            auto cls=v.inst->klass;
            while(cls){for(auto&[k,_]:cls->methods){if(!seen.count(k)){add(k);seen.insert(k);}}cls=cls->parent;}
        } else if (v.is_class()) {
            for (auto& [k,_]:v.cls->methods) add(k);
        } else if (v.is_list()) {
            for (auto& nm : {"length","append","find","pop","insert","sort","as_list"}) add(nm);
        } else if (v.is_str()) {
            for (auto& nm : {"length","lower","upper","strip","find","replace","split","startswith","endswith"}) add(nm);
        } else if (v.is_map()) {
            for (auto& nm : {"get","has","keys","values"}) add(nm);
        }
        return Value::make_list(list);
    });

    g["list_append"] = make_native("list_append", 2, [](std::vector<Value> args) {
        if (!args[0].is_list()) throw RuntimeError("list_append: first arg must be list");
        args[0].list->items.push_back(args[1]);
        return Value::make_null();
    });

    g["list_join"] = make_native("list_join", -1, [this](std::vector<Value> args) {
        if (args.empty() || !args[0].is_list()) throw RuntimeError("list_join: first arg must be list");
        std::string sep = args.size()>1 ? to_str(args[1]) : "";
        std::string result; bool first=true;
        for (auto& item : args[0].list->items) {
            if (!first) result += sep;
            result += to_str(item); first=false;
        }
        return Value::make_str(result);
    });

    g["min"] = make_native("min", 2, [this](std::vector<Value> args) {
        auto& a=args[0]; auto& b=args[1];
        if (a.is_inst()) {
            auto* m=a.inst->klass->find_method("<");
            if (m) { Value lt=call_method(a,"<",{b}); return is_truthy(lt)?a:b; }
        }
        if (a.is_num()&&b.is_num()) return a.n<=b.n?a:b;
        return a<b?a:b;
    });

    g["max"] = make_native("max", 2, [this](std::vector<Value> args) {
        auto& a=args[0]; auto& b=args[1];
        if (a.is_inst()) {
            auto* m=a.inst->klass->find_method("<");
            if (m) { Value lt=call_method(a,"<",{b}); return is_truthy(lt)?b:a; }
        }
        if (b.is_inst()) {
            auto* m=b.inst->klass->find_method("<");
            if (m) { Value lt=call_method(b,"<",{a}); return is_truthy(lt)?a:b; }
        }
        if (a.is_num()&&b.is_num()) return a.n>=b.n?a:b;
        return !(a<b)?a:b;
    });

    g["Fiber"] = make_native("Fiber", 1, [](std::vector<Value> args) {
        if (!args[0].is_callable()) throw RuntimeError("Fiber expects a callable");
        auto fib = std::make_shared<FiberData>();
        fib->fn_val = args[0];
        fib->state = FiberState::CREATED;
        return Value::make_fiber(fib);
    });

    // Type stubs
    auto make_type_class = [&](const std::string& nm) {
        auto cls = std::make_shared<ClassData>(); cls->name = nm;
        g[nm] = Value::make_class(cls);
    };
    make_type_class("String");
    make_type_class("List"); make_type_class("Map"); make_type_class("Range");

    // Stub modules
    auto make_stub = [&](const std::string& nm,
                         std::initializer_list<std::pair<const char*, Value>> attrs) {
        auto mod = std::make_shared<ModuleData>(); mod->name = nm;
        for (auto& [k,v] : attrs) mod->attrs[k] = v;
        Value mv = Value::make_mod(mod);
        modules_[nm] = mv;
        g[nm] = mv;
    };
    make_stub("lang", {{"clock", g["clock"]}});
    make_stub("io",   {{"write", make_native("write",-1,[this](std::vector<Value> a){
        for(auto& v:a) out_<<to_str(v); return Value::make_null();
    })}});
    make_stub("time", {{"sleep", make_native("sleep",1,[](std::vector<Value>){ return Value::make_null(); })}});
    make_stub("path", {{"sep", Value::make_str("/")}});
    make_stub("math", {
        {"pi",    Value::make_num(M_PI)},
        {"e",     Value::make_num(M_E)},
        {"floor", make_native("floor",1,[](std::vector<Value> a){ return Value::make_num(std::floor(a[0].n)); })},
        {"ceil",  make_native("ceil", 1,[](std::vector<Value> a){ return Value::make_num(std::ceil(a[0].n)); })},
        {"sqrt",  make_native("sqrt", 1,[](std::vector<Value> a){ return Value::make_num(std::sqrt(a[0].n)); })},
        {"abs",   make_native("abs",  1,[](std::vector<Value> a){ return Value::make_num(std::abs(a[0].n)); })},
    });
}


Value VM::run(std::shared_ptr<FuncChunk> chunk) {
    // Set up the initial frame for the script
    // Create a FuncData wrapper for the script chunk
    auto fn = std::make_shared<FuncData>();
    fn->name = chunk->name;
    fn->arity_val = 0;
    fn->chunk = chunk;

    // Push a dummy "callee" slot so RETURN's base-1 works
    push(Value::make_null()); // dummy callee at index 0
    size_t base = 1; // locals start at index 1 (but script has no locals, uses globals)

    CallFrame fr;
    fr.fn    = fn;
    fr.chunk = chunk;
    fr.ip    = 0;
    fr.base  = base;
    fr.self_val  = Value::make_null();
    fr.class_ptr = nullptr;
    frames_.push_back(std::move(fr));

    Value result = Value::make_null();
    run_loop(0, result);
    return result;
}

} // namespace pocketpp
