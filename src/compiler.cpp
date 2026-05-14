#include "compiler.hpp"
#include "errors.hpp"
#include "opcode.hpp"
#include <cmath>
#include <cassert>
#include <stdexcept>

namespace pocketpp {

// ── Emit helpers ──────────────────────────────────────────────────────────────

void Compiler::emit_op(Opcode op) {
    chunk().code.push_back((uint8_t)op);
    chunk().lines.push_back(current_line_);
}
void Compiler::emit_byte(uint8_t b) {
    chunk().code.push_back(b);
    chunk().lines.push_back(current_line_);
}
void Compiler::emit_u16(uint16_t v) {
    emit_byte((uint8_t)(v >> 8));
    emit_byte((uint8_t)(v & 0xff));
}
size_t Compiler::emit_jump(Opcode op) {
    emit_op(op);
    emit_byte(0xff); emit_byte(0xff);
    return chunk().code.size() - 2;
}
void Compiler::patch_jump(size_t off) {
    size_t dist = chunk().code.size() - off - 2;
    if (dist > 0xffff) throw ParseError("Jump too far");
    chunk().code[off]   = (uint8_t)(dist >> 8);
    chunk().code[off+1] = (uint8_t)(dist & 0xff);
}
void Compiler::emit_loop(size_t loop_start) {
    emit_op(Opcode::LOOP);
    size_t offset = chunk().code.size() + 2 - loop_start;
    if (offset > 0xffff) throw ParseError("Loop body too large");
    emit_byte((uint8_t)(offset >> 8));
    emit_byte((uint8_t)(offset & 0xff));
}

// ── Constant pool ─────────────────────────────────────────────────────────────

uint16_t Compiler::add_constant(Value v) {
    // reuse duplicate string/number constants
    if (v.is_str() || v.is_num()) {
        for (size_t i = 0; i < chunk().constants.size(); ++i)
            if (chunk().constants[i] == v) return (uint16_t)i;
    }
    chunk().constants.push_back(v);
    return (uint16_t)(chunk().constants.size() - 1);
}
uint16_t Compiler::str_const(const std::string& s) {
    return add_constant(Value::make_str(s));
}

// ── Variable resolution ───────────────────────────────────────────────────────

int Compiler::resolve_local(FnCtx& fn, const std::string& name) {
    for (int i = (int)fn.locals.size() - 1; i >= 0; --i)
        if (fn.locals[i].name == name) return i;
    return -1;
}

int Compiler::add_upvalue(FnCtx& fn, bool is_local, uint8_t idx) {
    for (int i = 0; i < (int)fn.upvals.size(); ++i)
        if (fn.upvals[i].is_local == is_local && fn.upvals[i].index == idx) return i;
    fn.upvals.push_back({is_local, idx});
    fn.chunk->upvalue_count = (int)fn.upvals.size();
    fn.chunk->upval_descs.push_back({is_local, idx});
    return (int)fn.upvals.size() - 1;
}

int Compiler::resolve_upvalue(int fn_idx, const std::string& name) {
    if (fn_idx <= 0) return -1;
    FnCtx& outer = fn_stack_[fn_idx - 1];
    if (outer.is_top_level) return -1; // top-level vars are globals, not upvalues
    int local = resolve_local(outer, name);
    if (local >= 0) {
        outer.locals[local].is_captured = true;
        return add_upvalue(fn_stack_[fn_idx], true, (uint8_t)local);
    }
    int upval = resolve_upvalue(fn_idx - 1, name);
    if (upval >= 0) {
        return add_upvalue(fn_stack_[fn_idx], false, (uint8_t)upval);
    }
    return -1;
}

void Compiler::emit_push_local(int slot) {
    if (slot <= 8) emit_op((Opcode)((int)Opcode::PUSH_LOCAL_0 + slot));
    else { emit_op(Opcode::PUSH_LOCAL_N); emit_byte((uint8_t)slot); }
}
void Compiler::emit_store_local(int slot) {
    if (slot <= 8) emit_op((Opcode)((int)Opcode::STORE_LOCAL_0 + slot));
    else { emit_op(Opcode::STORE_LOCAL_N); emit_byte((uint8_t)slot); }
}

void Compiler::emit_push_var(const std::string& name) {
    auto& fn = fn_ctx();
    // Always check locals first, even at top level (handles for-loop vars etc.)
    int local = resolve_local(fn, name);
    if (local >= 0) { emit_push_local(local); return; }
    if (!fn.is_top_level) {
        int uv = resolve_upvalue((int)fn_stack_.size() - 1, name);
        if (uv >= 0) { emit_op(Opcode::PUSH_UPVALUE); emit_byte((uint8_t)uv); return; }
    }
    emit_op(Opcode::PUSH_GLOBAL); emit_u16(str_const(name));
}

// define_new_local: true = first definition of this name as a local in this fn
// The RHS value is already on TOS; we either just record the local OR emit STORE_LOCAL
void Compiler::emit_store_var(const std::string& name, bool define_new_local) {
    auto& fn = fn_ctx();
    // Always check existing locals first (even at top level)
    int local = resolve_local(fn, name);
    if (local >= 0) { emit_store_local(local); return; }
    if (!fn.is_top_level) {
        int uv = resolve_upvalue((int)fn_stack_.size() - 1, name);
        if (uv >= 0) { emit_op(Opcode::STORE_UPVALUE); emit_byte((uint8_t)uv); return; }
        if (define_new_local) {
            // RHS is already at TOS which is the correct local slot; just record it
            fn.locals.push_back({name, fn.scope_depth, false});
            // No emit needed: the value is already in the right stack position
            return;
        }
        // New variable at function scope (not yet a local): DUP + record local
        // DUP so that ExprStmt's POP removes the copy while the original stays as the local
        emit_op(Opcode::DUP);
        fn.locals.push_back({name, fn.scope_depth, false});
        return;
    }
    emit_op(Opcode::STORE_GLOBAL); emit_u16(str_const(name));
}

// ── Scope management ──────────────────────────────────────────────────────────

void Compiler::begin_scope() { fn_ctx().scope_depth++; }
void Compiler::end_scope() {
    auto& fn = fn_ctx();
    fn.scope_depth--;
    pop_locals_to_depth(fn.scope_depth + 1);
}

void Compiler::pop_locals_to(int target_count) {
    auto& fn = fn_ctx();
    while ((int)fn.locals.size() > target_count) {
        auto& loc = fn.locals.back();
        if (loc.is_captured) emit_op(Opcode::CLOSE_UPVALUE);
        else                 emit_op(Opcode::POP);
        fn.locals.pop_back();
    }
}

// Pop locals that were added at scope depth > given depth
void Compiler::pop_locals_to_depth(int min_depth) {
    auto& fn = fn_ctx();
    int count = (int)fn.locals.size();
    while (count > 0 && fn.locals[count-1].scope_depth >= min_depth) {
        --count;
    }
    pop_locals_to(count);
}

// ── Opcode mapping ────────────────────────────────────────────────────────────

Opcode Compiler::tt_to_binary_op(TT t) {
    switch (t) {
    case TT::PLUS:      return Opcode::ADD;
    case TT::MINUS:     return Opcode::SUBTRACT;
    case TT::STAR:      return Opcode::MULTIPLY;
    case TT::SLASH:     return Opcode::DIVIDE;
    case TT::PERCENT:   return Opcode::MOD;
    case TT::STAR_STAR: return Opcode::EXPONENT;
    case TT::AMP:       return Opcode::BIT_AND;
    case TT::PIPE:      return Opcode::BIT_OR;
    case TT::CARET:     return Opcode::BIT_XOR;
    case TT::LSHIFT:    return Opcode::BIT_LSHIFT;
    case TT::RSHIFT:    return Opcode::BIT_RSHIFT;
    case TT::EQ_EQ:     return Opcode::EQEQ;
    case TT::BANG_EQ:   return Opcode::NOTEQ;
    case TT::LT:        return Opcode::LT;
    case TT::LT_EQ:     return Opcode::LTEQ;
    case TT::GT:        return Opcode::GT;
    case TT::GT_EQ:     return Opcode::GTEQ;
    case TT::DOT_DOT:   return Opcode::RANGE;
    default: throw ParseError("Unknown binary op in compiler");
    }
}

static Opcode compound_to_binary(TT t) {
    switch (t) {
    case TT::PLUS_EQ:      return Opcode::ADD;
    case TT::MINUS_EQ:     return Opcode::SUBTRACT;
    case TT::STAR_EQ:      return Opcode::MULTIPLY;
    case TT::SLASH_EQ:     return Opcode::DIVIDE;
    case TT::PERCENT_EQ:   return Opcode::MOD;
    case TT::STAR_STAR_EQ: return Opcode::EXPONENT;
    case TT::AMP_EQ:       return Opcode::BIT_AND;
    case TT::PIPE_EQ:      return Opcode::BIT_OR;
    case TT::CARET_EQ:     return Opcode::BIT_XOR;
    case TT::LSHIFT_EQ:    return Opcode::BIT_LSHIFT;
    case TT::RSHIFT_EQ:    return Opcode::BIT_RSHIFT;
    default: return Opcode::END; // not compound
    }
}

// ── Function compilation ──────────────────────────────────────────────────────

void Compiler::compile_function(const std::string& name,
                                 const std::vector<std::string>& params,
                                 const std::vector<StmtPtr>& body) {
    FnCtx fnc;
    fnc.chunk = std::make_shared<FuncChunk>();
    fnc.chunk->name  = name;
    fnc.chunk->arity = (int)params.size();
    fnc.is_top_level = false;
    fn_stack_.push_back(std::move(fnc));

    // Parameters are the first locals (they're already on the stack when called)
    for (auto& p : params)
        fn_ctx().locals.push_back({p, 0, false});

    // Strip leading docstring
    std::vector<StmtPtr> real_body = body;
    if (!real_body.empty()) {
        if (auto* es = dynamic_cast<ExprStmt*>(real_body[0].get()))
            if (auto* lit = dynamic_cast<LiteralExpr*>(es->expr.get()))
                if (lit->value.is_str()) {
                    fn_ctx().chunk->docs = lit->value.s;
                    real_body.erase(real_body.begin());
                }
    }

    compile_stmts(real_body);

    // Implicit return null
    emit_op(Opcode::PUSH_NULL);
    emit_op(Opcode::RETURN);

    // Capture the compiled chunk
    auto compiled = std::move(fn_ctx().chunk);
    auto upval_descs = fn_ctx().upvals;
    fn_stack_.pop_back();

    // Add chunk to outer's constants pool (push directly, no dedup)
    auto fd = std::make_shared<FuncData>();
    fd->name     = compiled->name;
    fd->arity_val = compiled->arity;
    fd->chunk    = compiled;
    fd->docs     = compiled->docs;
    chunk().constants.push_back(Value::make_fn(fd));
    uint16_t chunk_idx = (uint16_t)(chunk().constants.size() - 1);

    // Emit PUSH_CLOSURE opcode
    emit_op(Opcode::PUSH_CLOSURE);
    emit_u16(chunk_idx);
    // Followed by upvalue descriptors
    for (auto& ud : upval_descs) {
        emit_byte(ud.is_local ? 1 : 0);
        emit_byte(ud.index);
    }
}

// ── Compile assignment (name op= rhs) ─────────────────────────────────────────

void Compiler::compile_assign(const std::string& name, TT op_type, const ExprPtr& rhs_expr) {
    if (op_type != TT::ASSIGN) {
        // Compound: push current value, push rhs, binary op, store
        emit_push_var(name);
        compile_expr(rhs_expr);
        if (op_type == TT::PLUS_EQ)
            emit_op(Opcode::EXTEND_INPLACE);
        else
            emit_op(compound_to_binary(op_type));
        // store back (variable already exists since we just read it)
        auto& fn = fn_ctx();
        if (!fn.is_top_level) {
            int local = resolve_local(fn, name);
            if (local >= 0) { emit_store_local(local); return; }
            int uv = resolve_upvalue((int)fn_stack_.size()-1, name);
            if (uv >= 0) { emit_op(Opcode::STORE_UPVALUE); emit_byte((uint8_t)uv); return; }
        }
        emit_op(Opcode::STORE_GLOBAL); emit_u16(str_const(name));
        return;
    }
    // Simple assignment
    auto& fn = fn_ctx();
    if (!fn.is_top_level) {
        int local = resolve_local(fn, name);
        if (local >= 0) {
            compile_expr(rhs_expr);
            emit_store_local(local);
            return;
        }
        int uv = resolve_upvalue((int)fn_stack_.size()-1, name);
        if (uv >= 0) {
            compile_expr(rhs_expr);
            emit_op(Opcode::STORE_UPVALUE); emit_byte((uint8_t)uv);
            return;
        }
        // New local: compile rhs (pushes V at TOS = correct slot N), DUP, record local
        // DUP ensures ExprStmt's POP removes the copy while V stays as the local value
        compile_expr(rhs_expr);
        emit_op(Opcode::DUP);
        fn.locals.push_back({name, fn.scope_depth, false});
        return;
    }
    compile_expr(rhs_expr);
    emit_op(Opcode::STORE_GLOBAL); emit_u16(str_const(name));
}

// ── Compile statements ────────────────────────────────────────────────────────

void Compiler::compile_stmts(const std::vector<StmtPtr>& stmts) {
    for (auto& s : stmts) compile_stmt(s);
}

void Compiler::compile_stmt(const StmtPtr& s) {
    if (auto* es = dynamic_cast<ExprStmt*>(s.get())) {
        compile_expr(es->expr);
        emit_op(Opcode::POP);
        return;
    }

    if (auto* rs = dynamic_cast<ReturnStmt*>(s.get())) {
        if (rs->value) {
            // TCO: tail call optimization for direct (non-method, non-super) calls
            if (!fn_ctx().is_top_level) {
                if (auto* ce = dynamic_cast<CallExpr*>(rs->value.get())) {
                    if (!dynamic_cast<GetExpr*>(ce->callee.get()) &&
                        !dynamic_cast<SuperExpr*>(ce->callee.get())) {
                        compile_expr(ce->callee);
                        for (auto& a : ce->args) compile_expr(a);
                        emit_op(Opcode::TAIL_CALL);
                        emit_byte((uint8_t)ce->args.size());
                        return;
                    }
                }
            }
            compile_expr(rs->value);
        } else {
            emit_op(Opcode::PUSH_NULL);
        }
        emit_op(Opcode::RETURN);
        return;
    }

    if (dynamic_cast<BreakStmt*>(s.get())) {
        if (fn_ctx().loops.empty()) throw ParseError("break outside loop");
        auto& loop = fn_ctx().loops.back();
        // Pop all locals added since loop start
        pop_locals_to(loop.locals_count);
        size_t patch = emit_jump(Opcode::JUMP);
        loop.break_patches.push_back(patch);
        return;
    }

    if (auto* fs = dynamic_cast<FuncStmt*>(s.get())) {
        compile_function(fs->name, fs->params, fs->body);
        // PUSH_CLOSURE is already emitted; closure is on TOS
        auto& fn = fn_ctx();
        if (!fn.is_top_level) {
            int local = resolve_local(fn, fs->name);
            if (local >= 0) {
                // Update existing local: STORE copies TOS to slot, TOS stays, then POP
                emit_store_local(local);
                emit_op(Opcode::POP);
                return;
            }
            int uv = resolve_upvalue((int)fn_stack_.size()-1, fs->name);
            if (uv >= 0) {
                emit_op(Opcode::STORE_UPVALUE); emit_byte((uint8_t)uv);
                emit_op(Opcode::POP);
                return;
            }
            // New local: closure is at TOS = slot N; just record it — NO POP
            fn.locals.push_back({fs->name, fn.scope_depth, false});
            return;
        }
        // Global scope: STORE_GLOBAL leaves closure on TOS, then POP
        emit_op(Opcode::STORE_GLOBAL); emit_u16(str_const(fs->name));
        emit_op(Opcode::POP);
        return;
    }

    if (auto* cs = dynamic_cast<ClassStmt*>(s.get())) {
        // Push parent (or null)
        if (cs->parent) compile_expr(cs->parent);
        else            emit_op(Opcode::PUSH_NULL);
        // CREATE_CLASS pops parent, pushes class
        emit_op(Opcode::CREATE_CLASS);
        emit_u16(str_const(cs->name));
        // For each method: PUSH_CLOSURE then BIND_METHOD
        for (auto& method_stmt : cs->methods) {
            if (auto* mfs = dynamic_cast<FuncStmt*>(method_stmt.get())) {
                compile_function(mfs->name, mfs->params, mfs->body);
                // PUSH_CLOSURE is on TOS; class is below it; BIND_METHOD pops closure, leaves class
                emit_op(Opcode::BIND_METHOD);
                emit_u16(str_const(mfs->name));
            }
        }
        // Store class
        auto& fn = fn_ctx();
        if (!fn.is_top_level) {
            int local = resolve_local(fn, cs->name);
            if (local >= 0) {
                emit_store_local(local);
                emit_op(Opcode::POP);
                return;
            }
            int uv = resolve_upvalue((int)fn_stack_.size()-1, cs->name);
            if (uv >= 0) {
                emit_op(Opcode::STORE_UPVALUE); emit_byte((uint8_t)uv);
                emit_op(Opcode::POP);
                return;
            }
            // New local: class is at TOS = slot N; just record it — NO POP
            fn.locals.push_back({cs->name, fn.scope_depth, false});
            return;
        }
        // Global scope
        emit_op(Opcode::STORE_GLOBAL); emit_u16(str_const(cs->name));
        emit_op(Opcode::POP);
        return;
    }

    if (auto* ifs = dynamic_cast<IfStmt*>(s.get())) {
        std::vector<size_t> end_patches;
        for (size_t i = 0; i < ifs->branches.size(); ++i) {
            auto& br = ifs->branches[i];
            compile_expr(br.cond);
            size_t skip_patch = emit_jump(Opcode::JUMP_IF_NOT);
            begin_scope();
            compile_stmts(br.body);
            end_scope();
            if (i + 1 < ifs->branches.size() || !ifs->else_body.empty())
                end_patches.push_back(emit_jump(Opcode::JUMP));
            patch_jump(skip_patch);
        }
        if (!ifs->else_body.empty()) {
            begin_scope();
            compile_stmts(ifs->else_body);
            end_scope();
        }
        for (auto p : end_patches) patch_jump(p);
        return;
    }

    if (auto* ws = dynamic_cast<WhileStmt*>(s.get())) {
        LoopInfo li;
        li.start_offset  = chunk().code.size();
        li.locals_count  = (int)fn_ctx().locals.size();
        li.scope_depth   = fn_ctx().scope_depth;
        fn_ctx().loops.push_back(li);

        size_t loop_start = chunk().code.size();
        compile_expr(ws->cond);
        size_t exit_patch = emit_jump(Opcode::JUMP_IF_NOT);

        begin_scope();
        compile_stmts(ws->body);
        end_scope();

        emit_loop(loop_start);
        patch_jump(exit_patch);

        auto breaks = fn_ctx().loops.back().break_patches;
        fn_ctx().loops.pop_back();
        for (auto p : breaks) patch_jump(p);
        return;
    }

    if (auto* fors = dynamic_cast<ForStmt*>(s.get())) {
        // Stack layout: [container, counter(0), iter_item(= loop_var)]
        // Three hidden/named locals in current scope
        begin_scope(); // outer scope for iter vars

        compile_expr(fors->iter);  // push container
        emit_op(Opcode::PUSH_0);   // push counter = 0
        emit_op(Opcode::PUSH_NULL); // push current item (placeholder)

        // Define the 3 iter locals
        fn_ctx().locals.push_back({"__iter_obj__", fn_ctx().scope_depth, false});
        fn_ctx().locals.push_back({"__iter_cnt__", fn_ctx().scope_depth, false});
        fn_ctx().locals.push_back({fors->var,       fn_ctx().scope_depth, false});
        int loop_var_slot = (int)fn_ctx().locals.size() - 1;

        emit_op(Opcode::ITER_TEST); // validate container type

        LoopInfo li;
        li.start_offset = chunk().code.size();
        li.locals_count = (int)fn_ctx().locals.size(); // includes iter vars
        li.scope_depth  = fn_ctx().scope_depth;
        fn_ctx().loops.push_back(li);

        size_t loop_start = chunk().code.size();
        size_t iter_patch = emit_jump(Opcode::ITER); // ITER jumps if done

        begin_scope(); // inner scope for body vars
        compile_stmts(fors->body);
        end_scope();   // pop body vars

        // Mark loop var as captured if needed (check fn_ctx)
        // The LOOP_VAR slot might be captured; CLOSE is handled by end_scope for outer
        emit_loop(loop_start);
        patch_jump(iter_patch);

        auto breaks = fn_ctx().loops.back().break_patches;
        fn_ctx().loops.pop_back();
        for (auto p : breaks) patch_jump(p);

        end_scope(); // pop iter vars (container, counter, loop_var)
        return;
    }

    if (auto* is = dynamic_cast<ImportStmt*>(s.get())) {
        auto do_store = [&](const std::string& alias) {
            auto& fn = fn_ctx();
            if (!fn.is_top_level) {
                int local = resolve_local(fn, alias);
                if (local >= 0) { emit_store_local(local); emit_op(Opcode::POP); return; }
                int uv = resolve_upvalue((int)fn_stack_.size()-1, alias);
                if (uv >= 0) { emit_op(Opcode::STORE_UPVALUE); emit_byte((uint8_t)uv); emit_op(Opcode::POP); return; }
                int slot = (int)fn.locals.size();
                fn.locals.push_back({alias, fn.scope_depth, false});
                emit_store_local(slot);
                emit_op(Opcode::POP);
            } else {
                emit_op(Opcode::STORE_GLOBAL); emit_u16(str_const(alias));
                emit_op(Opcode::POP);
            }
        };

        if (is->is_from) {
            // from module import a, b as c
            emit_op(Opcode::IMPORT); emit_u16(str_const(is->module_path));
            // For each name:
            for (auto& na : is->names) {
                emit_op(Opcode::DUP);
                emit_op(Opcode::GET_ATTRIB); emit_u16(str_const(na.name));
                do_store(na.alias);
            }
            emit_op(Opcode::POP); // pop the module
        } else {
            // import module as alias
            auto do_import = [&](const std::string& path, const std::string& alias) {
                emit_op(Opcode::IMPORT); emit_u16(str_const(path));
                do_store(alias);
            };
            do_import(is->module_path, is->alias);
            for (auto& [p, a] : is->extra_imports) do_import(p, a);
        }
        return;
    }

    throw ParseError("Unknown statement type in compiler");
}

// ── Compile expressions ───────────────────────────────────────────────────────

void Compiler::compile_expr(const ExprPtr& e) {
    if (!e) { emit_op(Opcode::PUSH_NULL); return; }

    if (auto* lit = dynamic_cast<LiteralExpr*>(e.get())) {
        auto& v = lit->value;
        if (v.is_null())          { emit_op(Opcode::PUSH_NULL);  return; }
        if (v.is_bool() && v.b)   { emit_op(Opcode::PUSH_TRUE);  return; }
        if (v.is_bool() && !v.b)  { emit_op(Opcode::PUSH_FALSE); return; }
        if (v.is_num() && v.n==0) { emit_op(Opcode::PUSH_0);     return; }
        emit_op(Opcode::PUSH_CONSTANT); emit_u16(add_constant(v));
        return;
    }

    if (auto* g = dynamic_cast<GroupingExpr*>(e.get())) {
        compile_expr(g->expr); return;
    }

    if (auto* v = dynamic_cast<VariableExpr*>(e.get())) {
        current_line_ = v->name.line;
        emit_push_var(v->name.lexeme);
        return;
    }

    if (auto* sc = dynamic_cast<StringConcatExpr*>(e.get())) {
        for (auto& p : sc->parts) compile_expr(p);
        emit_op(Opcode::STRING_CONCAT);
        emit_byte((uint8_t)sc->parts.size());
        return;
    }

    if (auto* a = dynamic_cast<AssignExpr*>(e.get())) {
        current_line_ = a->name.line;
        compile_assign(a->name.lexeme, a->op.type, a->value);
        return;
    }

    if (auto* u = dynamic_cast<UnaryExpr*>(e.get())) {
        compile_expr(u->right);
        switch (u->op.type) {
        case TT::MINUS: emit_op(Opcode::NEGATIVE); break;
        case TT::BANG:
        case TT::NOT:   emit_op(Opcode::NOT);      break;
        case TT::TILDE: emit_op(Opcode::BIT_NOT);  break;
        default: throw ParseError("Unknown unary op");
        }
        return;
    }

    if (auto* b = dynamic_cast<BinaryExpr*>(e.get())) {
        compile_expr(b->left);
        compile_expr(b->right);
        emit_op(tt_to_binary_op(b->op.type));
        return;
    }

    if (auto* log = dynamic_cast<LogicalExpr*>(e.get())) {
        compile_expr(log->left);
        if (log->op.type == TT::OR) {
            size_t p = emit_jump(Opcode::OR);
            compile_expr(log->right);
            patch_jump(p);
        } else {
            size_t p = emit_jump(Opcode::AND);
            compile_expr(log->right);
            patch_jump(p);
        }
        return;
    }

    if (auto* idx = dynamic_cast<IndexExpr*>(e.get())) {
        compile_expr(idx->obj);
        compile_expr(idx->idx);
        emit_op(Opcode::GET_SUBSCRIPT);
        return;
    }

    if (auto* ia = dynamic_cast<IndexAssignExpr*>(e.get())) {
        compile_expr(ia->obj);
        compile_expr(ia->idx);
        if (ia->op.type != TT::ASSIGN) {
            // compound: obj idx → GET_SUBSCRIPT_KEEP → rhs → binary → SET_SUBSCRIPT
            emit_op(Opcode::GET_SUBSCRIPT_KEEP); // pushes value, obj+idx remain
            compile_expr(ia->val);
            emit_op(compound_to_binary(ia->op.type));
            emit_op(Opcode::SET_SUBSCRIPT); // pops value+idx, sets on obj, leaves value
        } else {
            compile_expr(ia->val);
            emit_op(Opcode::SET_SUBSCRIPT);
        }
        return;
    }

    if (auto* ge = dynamic_cast<GetExpr*>(e.get())) {
        compile_expr(ge->obj);
        emit_op(Opcode::GET_ATTRIB); emit_u16(str_const(ge->name.lexeme));
        return;
    }

    if (auto* se = dynamic_cast<SetExpr*>(e.get())) {
        compile_expr(se->obj);
        if (se->op.type != TT::ASSIGN) {
            // compound: GET_ATTRIB_KEEP then binary then SET_ATTRIB
            emit_op(Opcode::GET_ATTRIB_KEEP); emit_u16(str_const(se->name.lexeme));
            compile_expr(se->val);
            emit_op(compound_to_binary(se->op.type));
            emit_op(Opcode::SET_ATTRIB); emit_u16(str_const(se->name.lexeme));
        } else {
            compile_expr(se->val);
            emit_op(Opcode::SET_ATTRIB); emit_u16(str_const(se->name.lexeme));
        }
        return;
    }

    if (auto* ce = dynamic_cast<CallExpr*>(e.get())) {
        // Method call: obj.method(args)
        if (auto* get = dynamic_cast<GetExpr*>(ce->callee.get())) {
            compile_expr(get->obj);
            for (auto& a : ce->args) compile_expr(a);
            emit_op(Opcode::METHOD_CALL);
            emit_u16(str_const(get->name.lexeme));
            emit_byte((uint8_t)ce->args.size());
            return;
        }
        // Super call: super.method(args)
        if (auto* sup = dynamic_cast<SuperExpr*>(ce->callee.get())) {
            for (auto& a : ce->args) compile_expr(a);
            emit_op(Opcode::SUPER_CALL);
            emit_u16(str_const(sup->method));
            emit_byte((uint8_t)ce->args.size());
            return;
        }
        // Regular call
        compile_expr(ce->callee);
        for (auto& a : ce->args) compile_expr(a);
        emit_op(Opcode::CALL);
        emit_byte((uint8_t)ce->args.size());
        return;
    }

    if (auto* fn = dynamic_cast<FnExpr*>(e.get())) {
        compile_function("<fn>", fn->params, fn->body);
        // PUSH_CLOSURE is left on TOS
        return;
    }

    if (auto* le = dynamic_cast<ListExpr*>(e.get())) {
        for (auto& elem : le->elements) compile_expr(elem);
        emit_op(Opcode::PUSH_LIST);
        emit_u16((uint16_t)le->elements.size());
        return;
    }

    if (auto* me = dynamic_cast<MapExpr*>(e.get())) {
        emit_op(Opcode::PUSH_MAP);
        for (auto& [k,v] : me->pairs) {
            compile_expr(k);
            compile_expr(v);
            emit_op(Opcode::MAP_INSERT);
        }
        return;
    }

    if (dynamic_cast<SelfExpr*>(e.get())) {
        emit_op(Opcode::PUSH_SELF);
        return;
    }

    if (auto* sup = dynamic_cast<SuperExpr*>(e.get())) {
        // super used as expression (not call): push self for method lookup
        emit_op(Opcode::PUSH_SELF);
        return;
    }

    if (auto* ie = dynamic_cast<IsExpr*>(e.get())) {
        compile_expr(ie->obj);
        compile_expr(ie->cls);
        emit_op(Opcode::IS);
        return;
    }

    if (auto* ine = dynamic_cast<InExpr*>(e.get())) {
        compile_expr(ine->needle);
        compile_expr(ine->haystack);
        emit_op(ine->negated ? Opcode::NOT_IN : Opcode::IN);
        return;
    }

    if (auto* ye = dynamic_cast<YieldExpr*>(e.get())) {
        if (ye->value) compile_expr(ye->value);
        else           emit_op(Opcode::PUSH_NULL);
        emit_op(Opcode::YIELD);
        return;
    }

    throw ParseError("Unknown expression type in compiler");
}

// ── Entry point ───────────────────────────────────────────────────────────────

std::shared_ptr<FuncChunk> Compiler::compile_script(const std::vector<StmtPtr>& stmts) {
    // Reserve enough capacity so that fn_stack_ never reallocates while
    // compile_function recurses.  Without this, references like
    // `auto& fn = fn_ctx()` taken before a recursive compile_function call
    // become dangling when the vector moves its storage.
    fn_stack_.reserve(128);

    FnCtx fnc;
    fnc.chunk = std::make_shared<FuncChunk>();
    fnc.chunk->name  = "<script>";
    fnc.chunk->arity = 0;
    fnc.is_top_level = true;
    fn_stack_.push_back(std::move(fnc));

    compile_stmts(stmts);

    emit_op(Opcode::PUSH_NULL);
    emit_op(Opcode::RETURN);

    auto result = std::move(fn_ctx().chunk);
    fn_stack_.pop_back();
    return result;
}

} // namespace pocketpp
