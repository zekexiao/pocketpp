#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pocketpp {

// Forward-declare all complex types so Value can hold shared_ptr to them
struct ListData;
struct MapData;
struct FuncData;
struct FuncChunk;   // bytecode chunk
struct UpvalueSlot; // closure upvalue cell
struct ClassData;
struct InstanceData;
struct BoundMethodData;
struct FiberData;
struct ModuleData;

// ── Value ────────────────────────────────────────────────────────────────────
struct Value {
    enum class Type {
        Null, Bool, Number, String, List, Map, Range,
        Function, BoundMethod, Class, Instance, Fiber, Module
    };

    Type type{Type::Null};
    bool   b{false};
    double n{0.0};
    std::string s;
    std::pair<double,double> range{0.0, 0.0}; // first, last (exclusive end)
    std::shared_ptr<ListData>        list;
    std::shared_ptr<MapData>         map;
    std::shared_ptr<FuncData>        fn;
    std::shared_ptr<BoundMethodData> bound;
    std::shared_ptr<ClassData>       cls;
    std::shared_ptr<InstanceData>    inst;
    std::shared_ptr<FiberData>       fiber;
    std::shared_ptr<ModuleData>      mod;

    bool is_null()   const { return type == Type::Null; }
    bool is_bool()   const { return type == Type::Bool; }
    bool is_num()    const { return type == Type::Number; }
    bool is_str()    const { return type == Type::String; }
    bool is_list()   const { return type == Type::List; }
    bool is_map()    const { return type == Type::Map; }
    bool is_range()  const { return type == Type::Range; }
    bool is_fn()     const { return type == Type::Function; }
    bool is_bound()  const { return type == Type::BoundMethod; }
    bool is_class()  const { return type == Type::Class; }
    bool is_inst()   const { return type == Type::Instance; }
    bool is_fiber()  const { return type == Type::Fiber; }
    bool is_mod()    const { return type == Type::Module; }

    bool is_callable() const { return is_fn() || is_bound() || is_class(); }

    // Factory methods (defined after structs below)
    static Value make_null();
    static Value make_bool(bool v);
    static Value make_num(double v);
    static Value make_str(std::string v);
    static Value make_list(std::shared_ptr<ListData> d = nullptr);
    static Value make_map(std::shared_ptr<MapData> d = nullptr);
    static Value make_range(double a, double b);
    static Value make_fn(std::shared_ptr<FuncData> f);
    static Value make_bound(std::shared_ptr<BoundMethodData> bm);
    static Value make_class(std::shared_ptr<ClassData> c);
    static Value make_inst(std::shared_ptr<InstanceData> i);
    static Value make_fiber(std::shared_ptr<FiberData> f);
    static Value make_mod(std::shared_ptr<ModuleData> m);

    bool operator==(const Value& o) const;
    bool operator!=(const Value& o) const { return !(*this == o); }
    bool operator<(const Value& o) const;
};

// ── Now define the complex types (Value is complete here) ──────────────────

struct ListData {
    std::vector<Value> items;
};

struct MapData {
    // ordered by insertion
    std::vector<std::pair<Value, Value>> pairs;

    Value* find(const Value& key);
    const Value* find(const Value& key) const;
    void set(const Value& key, Value val);
    bool has(const Value& key) const { return find(key) != nullptr; }
};

struct FuncData {
    std::string name;
    int arity_val{0};   // -1 = variadic
    std::function<Value(std::vector<Value>)> call;   // native: non-null
    std::shared_ptr<FuncChunk> chunk;                // bytecode: non-null
    std::vector<std::shared_ptr<UpvalueSlot>> upvalues; // captured upvalues
    std::string docs;
    bool is_native() const { return (bool)call; }
};

struct ClassData {
    std::string name;
    std::shared_ptr<ClassData> parent;
    std::unordered_map<std::string, Value> methods;
    std::unordered_map<std::string, Value> class_attrs;
    std::string docs;

    const Value* find_method(const std::string& nm) const {
        auto it = methods.find(nm);
        if (it != methods.end()) return &it->second;
        if (parent) return parent->find_method(nm);
        return nullptr;
    }
};

struct InstanceData {
    std::shared_ptr<ClassData> klass;
    std::unordered_map<std::string, Value> attrs;
};

struct BoundMethodData {
    Value self_val;
    std::shared_ptr<FuncData> fn;
};

enum class FiberState { CREATED, RUNNING, SUSPENDED, DONE };

struct FiberData {
    Value fn_val;
    FiberState state{FiberState::CREATED};
    std::shared_ptr<void> impl; // actually FiberImpl*, defined in interpreter.cpp
};

struct ModuleData {
    std::string name;
    std::string path;
    std::unordered_map<std::string, Value> attrs;
    bool loading{false}; // cycle detection
};

// ── Bytecode chunk: one per function/script ──────────────────────────────────
struct FuncChunk {
    std::string name;
    int arity{0};      // -1 = variadic
    std::vector<uint8_t> code;
    std::vector<Value>   constants;
    std::vector<int>     lines;
    int upvalue_count{0};
    struct UpvalDesc { bool is_local; uint8_t index; };
    std::vector<UpvalDesc> upval_descs;
    std::string docs;
};

// ── Shared mutable cell for closure upvalues ──────────────────────────────────
struct UpvalueSlot {
    Value  closed;
    Value* location{nullptr};

    explicit UpvalueSlot(Value* loc) : location(loc) {}

    Value& deref() { return *location; }
    void close_over() {
        if (location != &closed) {
            closed = *location;
            location = &closed;
        }
    }
};

// ── Inline factory implementations ──────────────────────────────────────────
inline Value Value::make_null()                           { return Value{}; }
inline Value Value::make_bool(bool v)                     { Value r; r.type=Type::Bool;     r.b=v;              return r; }
inline Value Value::make_num(double v)                    { Value r; r.type=Type::Number;   r.n=v;              return r; }
inline Value Value::make_str(std::string v)               { Value r; r.type=Type::String;   r.s=std::move(v);   return r; }
inline Value Value::make_range(double a, double b)        { Value r; r.type=Type::Range;    r.range={a,b};      return r; }
inline Value Value::make_fn(std::shared_ptr<FuncData> f)  { Value r; r.type=Type::Function; r.fn=std::move(f);  return r; }
inline Value Value::make_bound(std::shared_ptr<BoundMethodData> bm){ Value r; r.type=Type::BoundMethod; r.bound=std::move(bm); return r; }
inline Value Value::make_class(std::shared_ptr<ClassData> c)        { Value r; r.type=Type::Class;       r.cls=std::move(c);  return r; }
inline Value Value::make_inst(std::shared_ptr<InstanceData> i)      { Value r; r.type=Type::Instance;    r.inst=std::move(i); return r; }
inline Value Value::make_fiber(std::shared_ptr<FiberData> f)        { Value r; r.type=Type::Fiber;       r.fiber=std::move(f);return r; }
inline Value Value::make_mod(std::shared_ptr<ModuleData> m)         { Value r; r.type=Type::Module;      r.mod=std::move(m);  return r; }

inline Value Value::make_list(std::shared_ptr<ListData> d) {
    Value r; r.type=Type::List;
    r.list = d ? std::move(d) : std::make_shared<ListData>();
    return r;
}
inline Value Value::make_map(std::shared_ptr<MapData> d) {
    Value r; r.type=Type::Map;
    r.map = d ? std::move(d) : std::make_shared<MapData>();
    return r;
}

inline Value* MapData::find(const Value& key) {
    for (auto& p : pairs)
        if (p.first == key) return &p.second;
    return nullptr;
}
inline const Value* MapData::find(const Value& key) const {
    for (const auto& p : pairs)
        if (p.first == key) return &p.second;
    return nullptr;
}
inline void MapData::set(const Value& key, Value val) {
    if (auto* v = find(key)) { *v = std::move(val); return; }
    pairs.emplace_back(key, std::move(val));
}

// ── Value equality / ordering ────────────────────────────────────────────────
inline bool Value::operator==(const Value& o) const {
    if (type != o.type) return false;
    switch (type) {
        case Type::Null:    return true;
        case Type::Bool:    return b == o.b;
        case Type::Number:  return n == o.n;
        case Type::String:  return s == o.s;
        case Type::Range:   return range == o.range;
        case Type::List: {
            if (list.get() == o.list.get()) return true;
            if (list->items.size() != o.list->items.size()) return false;
            for (size_t i = 0; i < list->items.size(); ++i)
                if (list->items[i] != o.list->items[i]) return false;
            return true;
        }
        case Type::Map: {
            if (map.get() == o.map.get()) return true;
            if (map->pairs.size() != o.map->pairs.size()) return false;
            for (auto& p : map->pairs) {
                const Value* v = o.map->find(p.first);
                if (!v || *v != p.second) return false;
            }
            return true;
        }
        case Type::Function:    return fn.get()    == o.fn.get();
        case Type::BoundMethod: return bound.get() == o.bound.get();
        case Type::Class:       return cls.get()   == o.cls.get();
        case Type::Instance:    return inst.get()  == o.inst.get();
        case Type::Fiber:       return fiber.get() == o.fiber.get();
        case Type::Module:      return mod.get()   == o.mod.get();
    }
    return false;
}

inline bool Value::operator<(const Value& o) const {
    if (type != o.type) return (int)type < (int)o.type;
    switch (type) {
        case Type::Number: return n < o.n;
        case Type::String: return s < o.s;
        case Type::Bool:   return (int)b < (int)o.b;
        default:           return false;
    }
}

} // namespace pocketpp
