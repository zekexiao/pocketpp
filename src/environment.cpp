#include "environment.hpp"
#include "errors.hpp"

namespace pocketpp {

Value Environment::get(const std::string& name) const {
    auto it = vars.find(name);
    if (it != vars.end()) return it->second;
    if (parent) return parent->get(name);
    throw RuntimeError("Undefined variable '" + name + "'.");
}

void Environment::set(const std::string& name, Value v) {
    // Walk up to find existing binding
    Environment* env = this;
    while (env) {
        auto it = env->vars.find(name);
        if (it != env->vars.end()) {
            it->second = std::move(v);
            return;
        }
        env = env->parent.get();
    }
    // Not found: create in current scope
    vars[name] = std::move(v);
}

} // namespace pocketpp
