#pragma once
#include "value.hpp"
#include <memory>
#include <string>
#include <unordered_map>

namespace pocketpp {

struct Environment : std::enable_shared_from_this<Environment> {
    std::unordered_map<std::string, Value> vars;
    std::shared_ptr<Environment> parent;

    explicit Environment(std::shared_ptr<Environment> p = nullptr)
        : parent(std::move(p)) {}

    // Look up a variable (walks chain)
    Value get(const std::string& name) const;

    // Assign to nearest scope where name exists; creates in current if not found
    void set(const std::string& name, Value v);

    // Define in current scope (no walk-up)
    void define(const std::string& name, Value v) { vars[name] = std::move(v); }

    bool has_local(const std::string& name) const { return vars.count(name) > 0; }
    bool has(const std::string& name) const {
        if (vars.count(name)) return true;
        if (parent) return parent->has(name);
        return false;
    }
};

} // namespace pocketpp
