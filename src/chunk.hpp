#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pocketpp {

// Forward declare Value so FuncChunk can hold a constants pool
struct Value;

// ── Bytecode chunk: one per function / script ─────────────────────────────
struct FuncChunk {
    std::string name;
    int arity{0};      // number of parameters (-1 = variadic)
    std::vector<uint8_t> code;
    std::vector<Value>   constants;   // constant pool
    std::vector<int>     lines;       // line[i] = source line for code[i]
    int upvalue_count{0};
    struct UpvalDesc { bool is_local; uint8_t index; };
    std::vector<UpvalDesc> upval_descs;
    std::string docs;
};

// ── A shared mutable cell for closure upvalues ────────────────────────────
struct UpvalueSlot {
    Value  closed;            // copy of value after it leaves the stack
    Value* location{nullptr}; // points into stack (open) or &closed (closed)

    explicit UpvalueSlot(Value* loc) : location(loc) {}

    Value read() const;
    void  write(const Value& v);
    void  close_over();
};

} // namespace pocketpp
