#pragma once
#include "ast.hpp"
#include "opcode.hpp"
#include "value.hpp"
#include <memory>
#include <string>
#include <vector>

namespace pocketpp {

class Compiler {
public:
    // Compile a list of top-level statements into a "script" chunk.
    // All top-level variables are globals.
    std::shared_ptr<FuncChunk> compile_script(const std::vector<StmtPtr>& stmts);

private:
    // ── Per-function compiler state ───────────────────────────────────────
    struct Local {
        std::string name;
        int  scope_depth{0};
        bool is_captured{false};
    };
    struct UpvalInfo {
        bool    is_local;
        uint8_t index;
    };
    struct LoopInfo {
        size_t start_offset;
        int    locals_count;
        int    scope_depth;
        std::vector<size_t> break_patches;
    };
    struct FnCtx {
        std::shared_ptr<FuncChunk> chunk;
        std::vector<Local>    locals;
        std::vector<UpvalInfo> upvals;
        int scope_depth{0};
        bool is_top_level{false};
        std::vector<LoopInfo> loops;
    };

    std::vector<FnCtx> fn_stack_;
    int current_line_{0};

    FnCtx&     fn_ctx()  { return fn_stack_.back(); }
    FuncChunk& chunk()   { return *fn_ctx().chunk; }

    // ── Emit helpers ──────────────────────────────────────────────────────
    void     emit_op(Opcode op);
    void     emit_byte(uint8_t b);
    void     emit_u16(uint16_t v);
    size_t   emit_jump(Opcode op);          // returns patch offset
    void     patch_jump(size_t off);        // patch to current position
    void     emit_loop(size_t loop_start);  // backward jump

    // ── Constant pool ─────────────────────────────────────────────────────
    uint16_t add_constant(Value v);
    uint16_t str_const(const std::string& s);

    // ── Variable resolution ───────────────────────────────────────────────
    int  resolve_local(FnCtx& fn, const std::string& name);
    int  resolve_upvalue(int fn_idx, const std::string& name);
    int  add_upvalue(FnCtx& fn, bool is_local, uint8_t idx);
    void emit_push_var(const std::string& name);
    void emit_store_var(const std::string& name, bool define_new_local);
    void emit_push_local(int slot);
    void emit_store_local(int slot);

    // ── Scope management ─────────────────────────────────────────────────
    void begin_scope();
    void end_scope();
    // Pop locals down to (not including) target_count, emitting CLOSE_UPVALUE or POP
    void pop_locals_to(int target_count);
    // Pop locals whose scope_depth >= min_depth
    void pop_locals_to_depth(int min_depth);

    // ── Compilation visitors ──────────────────────────────────────────────
    void compile_stmts(const std::vector<StmtPtr>& stmts);
    void compile_stmt(const StmtPtr& s);
    void compile_expr(const ExprPtr& e);
    void compile_function(const std::string& name,
                          const std::vector<std::string>& params,
                          const std::vector<StmtPtr>& body);
    void compile_assign(const std::string& name, TT op_type, const ExprPtr& rhs_expr);
    Opcode tt_to_binary_op(TT t);
};

} // namespace pocketpp
