#include "parser.hpp"
#include "errors.hpp"
#include <memory>

namespace pocketpp {

Token Parser::expect(TT t, const std::string& msg) {
    if (!check(t)) {
        auto& c = cur();
        throw ParseError("[line " + std::to_string(c.line) + "] " + msg +
                         " (got '" + c.lexeme + "')");
    }
    return advance();
}

void Parser::skip_newlines() {
    while (check(TT::NEWLINE)) advance();
}

std::vector<StmtPtr> Parser::parse() {
    std::vector<StmtPtr> stmts;
    skip_newlines();
    while (!at_end()) {
        stmts.push_back(statement());
        while (check(TT::NEWLINE)) advance();
    }
    return stmts;
}

std::vector<StmtPtr> Parser::block(TT end_tok) {
    std::vector<StmtPtr> stmts;
    skip_newlines();
    while (!at_end() && !check(end_tok) && !check(TT::ELIF) && !check(TT::ELSE)) {
        stmts.push_back(statement());
        skip_newlines();
    }
    return stmts;
}

std::vector<StmtPtr> Parser::func_body() {
    std::vector<StmtPtr> stmts;
    skip_newlines();
    while (!at_end() && !check(TT::END)) {
        stmts.push_back(statement());
        skip_newlines();
    }
    expect(TT::END, "Expected 'end'");
    return stmts;
}

StmtPtr Parser::statement() {
    skip_newlines();
    if (check(TT::DEF))    return def_stmt();
    if (check(TT::CLASS))  return class_stmt();
    if (check(TT::IF))     return if_stmt();
    if (check(TT::WHILE))  return while_stmt();
    if (check(TT::FOR))    return for_stmt();
    if (check(TT::RETURN)) return return_stmt();
    if (check(TT::BREAK))  { advance(); match(TT::NEWLINE); return std::make_shared<BreakStmt>(); }
    if (check(TT::IMPORT)||check(TT::FROM)) return import_stmt();
    return expr_stmt();
}

StmtPtr Parser::def_stmt() {
    advance(); // consume 'def'
    // Accept operator tokens as method names
    std::string fname;
    static const TT op_tts[] = {
        TT::PLUS,TT::MINUS,TT::STAR,TT::SLASH,TT::PERCENT,
        TT::STAR_STAR,TT::PLUS_EQ,TT::MINUS_EQ,TT::STAR_EQ,TT::SLASH_EQ,
        TT::PERCENT_EQ,TT::STAR_STAR_EQ,TT::EQ_EQ,TT::BANG_EQ,
        TT::LT,TT::LT_EQ,TT::GT,TT::GT_EQ,TT::BANG,TT::NOT,
        TT::AMP,TT::PIPE,TT::CARET,TT::LSHIFT,TT::RSHIFT,
        TT::AMP_EQ,TT::PIPE_EQ,TT::CARET_EQ,TT::LSHIFT_EQ,TT::RSHIFT_EQ,
        TT::TILDE,TT::DOT_DOT
    };
    bool is_op = false;
    for (auto ott : op_tts) { if (check(ott)) { fname = advance().lexeme; is_op=true; break; } }
    if (!is_op) fname = expect(TT::IDENT, "Expected function name").lexeme;
    std::vector<std::string> params;
    if (match(TT::LPAREN)) {
        if (!check(TT::RPAREN)) {
            do {
                skip_newlines();
                params.push_back(expect(TT::IDENT, "Expected parameter name").lexeme);
            } while (match(TT::COMMA));
        }
        expect(TT::RPAREN, "Expected ')'");
    }
    skip_newlines();
    auto body = func_body();
    auto s = std::make_shared<FuncStmt>();
    s->name = fname;
    s->params = std::move(params);
    s->body = std::move(body);
    return s;
}

StmtPtr Parser::class_stmt() {
    advance(); // consume 'class'
    auto name_tok = expect(TT::IDENT, "Expected class name");
    auto stmt = std::make_shared<ClassStmt>();
    stmt->name = name_tok.lexeme;
    if (match(TT::IS)) {
        stmt->parent = primary();
    }
    skip_newlines();
    // Check for class-level docstring
    if (check(TT::STRING)) {
        stmt->docs = advance().lexeme;
        skip_newlines();
    }
    while (!at_end() && !check(TT::END)) {
        skip_newlines();
        if (check(TT::END)) break;
        stmt->methods.push_back(statement());
        skip_newlines();
    }
    expect(TT::END, "Expected 'end' after class body");
    return stmt;
}

StmtPtr Parser::if_stmt() {
    advance(); // consume 'if'
    auto stmt = std::make_shared<IfStmt>();
    {
        IfStmt::Branch b;
        b.cond = expression();
        match(TT::THEN);
        skip_newlines();
        b.body = block(TT::END);
        stmt->branches.push_back(std::move(b));
    }
    while (check(TT::ELIF)) {
        advance();
        IfStmt::Branch b;
        b.cond = expression();
        match(TT::THEN);
        skip_newlines();
        b.body = block(TT::END);
        stmt->branches.push_back(std::move(b));
    }
    if (match(TT::ELSE)) {
        skip_newlines();
        stmt->else_body = block(TT::END);
    }
    expect(TT::END, "Expected 'end' after if");
    return stmt;
}

StmtPtr Parser::while_stmt() {
    advance(); // consume 'while'
    auto stmt = std::make_shared<WhileStmt>();
    stmt->cond = expression();
    match(TT::DO);
    skip_newlines();
    stmt->body = block(TT::END);
    expect(TT::END, "Expected 'end' after while");
    return stmt;
}

StmtPtr Parser::for_stmt() {
    advance(); // consume 'for'
    auto stmt = std::make_shared<ForStmt>();
    stmt->var = expect(TT::IDENT, "Expected loop variable").lexeme;
    expect(TT::IN, "Expected 'in'");
    stmt->iter = expression();
    match(TT::DO);
    skip_newlines();
    stmt->body = block(TT::END);
    expect(TT::END, "Expected 'end' after for");
    return stmt;
}

StmtPtr Parser::return_stmt() {
    advance(); // consume 'return'
    ExprPtr val;
    if (!check(TT::NEWLINE) && !check(TT::EOFILE) && !check(TT::END)) {
        val = expression();
    }
    match(TT::NEWLINE);
    return std::make_shared<ReturnStmt>(std::move(val));
}

// Helper to parse a dotted module path (with optional ^^ prefix)
static std::string parse_module_path(Parser& /*p*/, std::vector<Token>& tokens, size_t& pos) {
    // Not used
    return "";
}

StmtPtr Parser::import_stmt() {
    // Returns a multi-import as a "block" - actually we need to handle multiple stmts
    // For `import a, b` we'll return a BlockStmt-equivalent by using a single ImportStmt
    // with multiple entries. For now, parse all on one line.
    auto stmt = std::make_shared<ImportStmt>();
    if (match(TT::FROM)) {
        stmt->is_from = true;
        std::string path;
        // Handle ^^ relative imports
        while (check(TT::CARET)) { advance(); path += "^"; }
        if (check(TT::IDENT)) path += advance().lexeme;
        while (match(TT::DOT)) {
            if (check(TT::IDENT)) path += "." + advance().lexeme;
        }
        stmt->module_path = path;
        expect(TT::IMPORT, "Expected 'import'");
        do {
            skip_newlines();
            ImportStmt::NameAlias na;
            na.name = expect(TT::IDENT, "Expected name").lexeme;
            if (match(TT::AS)) na.alias = expect(TT::IDENT,"Expected alias").lexeme;
            else na.alias = na.name;
            stmt->names.push_back(na);
        } while (match(TT::COMMA));
    } else {
        advance(); // 'import'
        // Parse first module
        auto parse_one = [&]() -> std::pair<std::string, std::string> {
            std::string path;
            // Handle ^^ relative imports
            while (check(TT::CARET)) { advance(); path += "^"; }
            if (check(TT::IDENT)) path += advance().lexeme;
            while (match(TT::DOT)) {
                if(check(TT::IDENT)) path += "." + advance().lexeme;
            }
            std::string alias;
            if (match(TT::AS)) alias = expect(TT::IDENT,"Expected alias").lexeme;
            else {
                auto dot = path.rfind('.');
                alias = dot == std::string::npos ? path : path.substr(dot+1);
                // Remove ^^ from alias
                while (!alias.empty() && alias[0]=='^') alias = alias.substr(1);
            }
            return {path, alias};
        };
        auto [p0, a0] = parse_one();
        stmt->module_path = p0;
        stmt->alias = a0;
        // Handle multiple imports: `import a, b as c`
        // We store extra ones in extra_imports
        while (match(TT::COMMA)) {
            skip_newlines();
            auto [p, a] = parse_one();
            stmt->extra_imports.push_back({p, a});
        }
    }
    match(TT::NEWLINE);
    return stmt;
}

StmtPtr Parser::expr_stmt() {
    auto e = expression();
    match(TT::NEWLINE);
    return std::make_shared<ExprStmt>(std::move(e));
}

ExprPtr Parser::expression() { return assignment(); }

ExprPtr Parser::assignment() {
    auto left = or_expr();
    static const TT compound_ops[] = {
        TT::ASSIGN,TT::PLUS_EQ,TT::MINUS_EQ,TT::STAR_EQ,TT::SLASH_EQ,
        TT::PERCENT_EQ,TT::STAR_STAR_EQ,TT::AMP_EQ,TT::PIPE_EQ,
        TT::CARET_EQ,TT::LSHIFT_EQ,TT::RSHIFT_EQ
    };
    for (auto op : compound_ops) {
        if (check(op)) {
            Token op_tok = advance();
            skip_newlines();
            auto val = assignment();
            if (auto* var = dynamic_cast<VariableExpr*>(left.get()))
                return std::make_shared<AssignExpr>(var->name, op_tok, std::move(val));
            if (auto* idx = dynamic_cast<IndexExpr*>(left.get()))
                return std::make_shared<IndexAssignExpr>(idx->obj, idx->idx, op_tok, std::move(val));
            if (auto* get = dynamic_cast<GetExpr*>(left.get()))
                return std::make_shared<SetExpr>(get->obj, get->name, op_tok, std::move(val));
            throw ParseError("Invalid assignment target");
        }
    }
    return left;
}

ExprPtr Parser::or_expr() {
    auto l = and_expr();
    while (check(TT::OR)) {
        Token op = advance(); skip_newlines();
        auto r = and_expr();
        l = std::make_shared<LogicalExpr>(std::move(l),op,std::move(r));
    }
    return l;
}
ExprPtr Parser::and_expr() {
    auto l = not_expr();
    while (check(TT::AND)) {
        Token op = advance(); skip_newlines();
        auto r = not_expr();
        l = std::make_shared<LogicalExpr>(std::move(l),op,std::move(r));
    }
    return l;
}
ExprPtr Parser::not_expr() {
    if (check(TT::NOT)) { Token op = advance(); return std::make_shared<UnaryExpr>(op, not_expr()); }
    return comparison();
}
ExprPtr Parser::comparison() {
    auto l = bitwise_or();
    while (true) {
        if (check(TT::EQ_EQ)||check(TT::BANG_EQ)||
            check(TT::LT)||check(TT::LT_EQ)||check(TT::GT)||check(TT::GT_EQ)) {
            Token op = advance(); skip_newlines(); auto r = bitwise_or();
            l = std::make_shared<BinaryExpr>(std::move(l),op,std::move(r));
        } else if (check(TT::IS)) {
            advance(); auto r = bitwise_or();
            l = std::make_shared<IsExpr>(std::move(l), std::move(r));
        } else if (check(TT::IN)) {
            advance(); auto r = bitwise_or();
            l = std::make_shared<InExpr>(std::move(l), std::move(r));
        } else if (check(TT::NOT) && pos_+1<tokens_.size() && tokens_[pos_+1].type==TT::IN) {
            advance(); advance(); auto r = bitwise_or();
            l = std::make_shared<InExpr>(std::move(l), std::move(r), true);
        } else break;
    }
    return l;
}
ExprPtr Parser::bitwise_or() {
    auto l = bitwise_xor();
    while (check(TT::PIPE)) { Token op=advance(); skip_newlines(); auto r=bitwise_xor(); l=std::make_shared<BinaryExpr>(std::move(l),op,std::move(r)); }
    return l;
}
ExprPtr Parser::bitwise_xor() {
    auto l = bitwise_and();
    while (check(TT::CARET)) { Token op=advance(); skip_newlines(); auto r=bitwise_and(); l=std::make_shared<BinaryExpr>(std::move(l),op,std::move(r)); }
    return l;
}
ExprPtr Parser::bitwise_and() {
    auto l = shift();
    while (check(TT::AMP)) { Token op=advance(); skip_newlines(); auto r=shift(); l=std::make_shared<BinaryExpr>(std::move(l),op,std::move(r)); }
    return l;
}
ExprPtr Parser::shift() {
    auto l = range_expr();
    while (check(TT::LSHIFT)||check(TT::RSHIFT)) { Token op=advance(); skip_newlines(); auto r=range_expr(); l=std::make_shared<BinaryExpr>(std::move(l),op,std::move(r)); }
    return l;
}
ExprPtr Parser::range_expr() {
    auto l = concat();
    while (check(TT::DOT_DOT)) { Token op=advance(); skip_newlines(); auto r=concat(); l=std::make_shared<BinaryExpr>(std::move(l),op,std::move(r)); }
    return l;
}
ExprPtr Parser::concat() { return additive(); }
ExprPtr Parser::additive() {
    auto l = multiplicative();
    while (check(TT::PLUS)||check(TT::MINUS)) {
        Token op=advance(); skip_newlines();
        auto r=multiplicative(); l=std::make_shared<BinaryExpr>(std::move(l),op,std::move(r));
    }
    return l;
}
ExprPtr Parser::multiplicative() {
    auto l = unary();
    while (check(TT::STAR)||check(TT::SLASH)||check(TT::PERCENT)) {
        Token op=advance(); auto r=unary(); l=std::make_shared<BinaryExpr>(std::move(l),op,std::move(r));
    }
    return l;
}
ExprPtr Parser::power() {
    // power_base: postfix then optional **
    auto e = primary();
    e = postfix(std::move(e));
    if (check(TT::STAR_STAR)) { Token op=advance(); skip_newlines(); auto r=unary(); return std::make_shared<BinaryExpr>(std::move(e),op,std::move(r)); }
    return e;
}
ExprPtr Parser::unary() {
    if (check(TT::MINUS)||check(TT::BANG)||check(TT::TILDE)||check(TT::NOT)) {
        Token op=advance(); return std::make_shared<UnaryExpr>(op, unary());
    }
    return power(); // power includes postfix
}
ExprPtr Parser::postfix(ExprPtr left) {
    while (true) {
        if (check(TT::LPAREN)) {
            left = call_expr(std::move(left));
        } else if (check(TT::LBRACKET)) {
            Token bracket = advance();
            skip_newlines();
            auto idx = expression();
            skip_newlines();
            expect(TT::RBRACKET,"Expected ']'");
            left = std::make_shared<IndexExpr>(std::move(left), std::move(idx), bracket);
        } else if (check(TT::DOT)) {
            advance();
            auto name_tok = expect(TT::IDENT,"Expected attribute name");
            if (check(TT::LPAREN)) {
                auto get = std::make_shared<GetExpr>(std::move(left), name_tok);
                left = call_expr(std::move(get));
            } else if (check(TT::FN)) {
                // Trailing fn argument: obj.method fn(params) body end
                auto get = std::make_shared<GetExpr>(std::move(left), name_tok);
                Token fake_paren{TT::LPAREN,"(",0,name_tok.line};
                std::vector<ExprPtr> fn_args;
                fn_args.push_back(fn_expr());
                left = std::make_shared<CallExpr>(std::move(get), std::move(fn_args), fake_paren);
            } else {
                left = std::make_shared<GetExpr>(std::move(left), name_tok);
            }
        } else if (check(TT::FN)) {
            // Trailing fn argument to any callee: expr fn(params) body end
            Token fake_paren{TT::LPAREN,"(",0,0};
            std::vector<ExprPtr> fn_args;
            fn_args.push_back(fn_expr());
            left = std::make_shared<CallExpr>(std::move(left), std::move(fn_args), fake_paren);
        } else break;
    }
    return left;
}
ExprPtr Parser::call_expr(ExprPtr callee) {
    Token paren = advance(); // '('
    std::vector<ExprPtr> args;
    skip_newlines();
    if (!check(TT::RPAREN)) {
        do { skip_newlines(); args.push_back(expression()); skip_newlines(); } while (match(TT::COMMA));
    }
    expect(TT::RPAREN,"Expected ')'");
    return std::make_shared<CallExpr>(std::move(callee), std::move(args), paren);
}
ExprPtr Parser::fn_expr() {
    advance(); // 'fn'
    std::vector<std::string> params;
    if (match(TT::LPAREN)) {
        if (!check(TT::RPAREN)) {
            do { params.push_back(expect(TT::IDENT,"Expected param").lexeme); } while (match(TT::COMMA));
        }
        expect(TT::RPAREN,"Expected ')'");
    }
    skip_newlines();
    auto body = func_body();
    return std::make_shared<FnExpr>(std::move(params), std::move(body));
}
ExprPtr Parser::primary() {
    if (check(TT::KW_NULL))  { advance(); return std::make_shared<LiteralExpr>(Value::make_null()); }
    if (check(TT::KW_TRUE))  { advance(); return std::make_shared<LiteralExpr>(Value::make_bool(true)); }
    if (check(TT::KW_FALSE)) { advance(); return std::make_shared<LiteralExpr>(Value::make_bool(false)); }
    if (check(TT::NUMBER))   { Token t=advance(); return std::make_shared<LiteralExpr>(Value::make_num(t.num_val)); }
    if (check(TT::STRING))   { Token t=advance(); return std::make_shared<LiteralExpr>(Value::make_str(t.lexeme)); }
    if (check(TT::SELF))     { Token t=advance(); return std::make_shared<SelfExpr>(t); }
    if (check(TT::SUPER)) {
        Token kw=advance();
        std::string method_name;
        if (match(TT::DOT)) method_name = expect(TT::IDENT,"Expected method name").lexeme;
        return std::make_shared<SuperExpr>(kw, method_name, false);
    }
    if (check(TT::FN)) return fn_expr();
    if (check(TT::LPAREN)) {
        advance(); skip_newlines();
        auto e = expression();
        skip_newlines();
        expect(TT::RPAREN,"Expected ')'");
        return std::make_shared<GroupingExpr>(std::move(e));
    }
    if (check(TT::LBRACKET)) {
        advance(); skip_newlines();
        std::vector<ExprPtr> elems;
        if (!check(TT::RBRACKET)) {
            do { skip_newlines(); if(check(TT::RBRACKET))break; elems.push_back(expression()); skip_newlines(); } while (match(TT::COMMA));
        }
        expect(TT::RBRACKET,"Expected ']'");
        return std::make_shared<ListExpr>(std::move(elems));
    }
    if (check(TT::LBRACE)) {
        advance(); skip_newlines();
        std::vector<std::pair<ExprPtr,ExprPtr>> pairs;
        if (!check(TT::RBRACE)) {
            do {
                skip_newlines(); if(check(TT::RBRACE))break;
                auto k=expression(); expect(TT::COLON,"Expected ':'"); skip_newlines(); auto v=expression();
                pairs.emplace_back(std::move(k),std::move(v)); skip_newlines();
            } while (match(TT::COMMA));
        }
        expect(TT::RBRACE,"Expected '}'");
        return std::make_shared<MapExpr>(std::move(pairs));
    }
    if (check(TT::YIELD)) {
        advance();
        ExprPtr val;
        if (check(TT::LPAREN)) {
            // yield() or yield(expr) - handle parens explicitly
            advance(); // consume '('
            if (!check(TT::RPAREN)) {
                val = expression();
            }
            expect(TT::RPAREN, "Expected ')'");
        } else if (!check(TT::NEWLINE)&&!check(TT::RPAREN)&&!check(TT::EOFILE)&&!check(TT::SEMICOLON)) {
            val = expression();
        }
        return std::make_shared<YieldExpr>(std::move(val));
    }
    if (check(TT::IDENT)) { Token t=advance(); return std::make_shared<VariableExpr>(t); }
    auto& c = cur();
    throw ParseError("[line "+std::to_string(c.line)+"] Unexpected token '"+c.lexeme+"'");
}

} // namespace pocketpp
