#pragma once
#include "errors.hpp"
#include "opcode.hpp"
#include "value.hpp"
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace pocketpp {

struct FiberImpl;

struct CallFrame {
    std::shared_ptr<FuncData>  fn;
    std::shared_ptr<FuncChunk> chunk;
    size_t ip{0};
    size_t base{0};      // index of first local/arg in stack_
    Value  self_val;
    std::shared_ptr<ClassData> class_ptr;
    // For class instantiation: after _init returns push this instead of _init's null return
    bool  has_post_call{false};
    Value post_call_value;
};

class VM {
public:
    VM(std::unordered_map<std::string, Value>& globals,
       std::ostringstream& out,
       const std::string& base_dir,
       std::unordered_map<std::string, Value>& modules);

    // Run the script chunk (no args, no self)
    Value run(std::shared_ptr<FuncChunk> chunk);

    void register_builtins();

    // Public helpers (used by native builtins, fibers)
    Value call_value(const Value& callee, std::vector<Value> args);
    bool  is_truthy(const Value& v);
    std::string to_str(const Value& v, std::vector<const void*>* seen = nullptr);
    Value make_native(const std::string& nm, int ar,
                      std::function<Value(std::vector<Value>)> fn);

    // Current fiber context (null when not in a fiber)
    FiberImpl* current_fiber_{nullptr};

private:
    std::vector<Value>     stack_;
    std::vector<CallFrame> frames_;
    std::vector<std::shared_ptr<UpvalueSlot>> open_upvalues_;

    std::unordered_map<std::string, Value>& globals_;
    std::ostringstream& out_;
    std::string base_dir_;
    std::unordered_map<std::string, Value>& modules_;

    static constexpr size_t STACK_MAX = 65536;

    void   push(Value v);
    Value  pop();
    Value& peek(size_t dist = 0);   // 0 = TOS
    size_t sp() const { return stack_.size(); }

    CallFrame& frame() { return frames_.back(); }
    uint8_t  read_byte();
    uint16_t read_u16();
    int      line();

    std::shared_ptr<UpvalueSlot> capture_upvalue(size_t abs_slot);
    void close_upvalues(size_t min_slot);

    // Call helpers
    void do_call(const Value& callee, int argc, bool is_tail = false);
    void do_method_call(const std::string& name, int argc);
    void do_super_call(const std::string& name, int argc);

    // Runtime operations
    Value do_binary(Opcode op, Value l, Value r);
    Value do_get_attrib(const Value& obj, const std::string& name);
    void  do_set_attrib(Value& obj, const std::string& name, Value val);
    Value do_subscript(const Value& obj, const Value& idx);
    void  do_subscript_assign(Value& obj, const Value& idx, Value val);
    Value do_slice(const Value& obj, double a, double b);

    Value call_method(Value self, const std::string& name, std::vector<Value> args);
    Value get_attr(const Value& obj, const std::string& name);

    Value import_module(const std::string& path);
    void  exec_module(const std::string& path, ModuleData& mod);

    // Inner execution
    void  run_loop(size_t target_depth, Value& ret_val);
    Value run_inner();
};

} // namespace pocketpp
