#pragma once
#include "ast.hpp"
#include "token.hpp"
#include <vector>

namespace pocketpp {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}
    std::vector<StmtPtr> parse();

private:
    std::vector<Token> tokens_;
    size_t pos_{0};

    const Token& cur()  const { return tokens_[pos_]; }
    const Token& prev() const { return tokens_[pos_>0?pos_-1:0]; }
    bool at_end()       const { return cur().type == TT::EOFILE; }
    bool check(TT t)    const { return cur().type == t; }

    Token advance() {
        Token t = tokens_[pos_];
        if (!at_end()) ++pos_;
        return t;
    }
    bool match(TT t) { if(check(t)){advance();return true;}return false; }
    bool match2(TT a,TT b){ if(check(a)||check(b)){advance();return true;}return false; }
    Token expect(TT t, const std::string& msg);
    void skip_newlines();

    std::vector<StmtPtr> block(TT end_tok = TT::END); // reads until end_tok
    StmtPtr statement();
    StmtPtr def_stmt();
    StmtPtr class_stmt();
    StmtPtr if_stmt();
    StmtPtr while_stmt();
    StmtPtr for_stmt();
    StmtPtr return_stmt();
    StmtPtr import_stmt();
    StmtPtr expr_stmt();

    ExprPtr expression();
    ExprPtr assignment();
    ExprPtr or_expr();
    ExprPtr and_expr();
    ExprPtr not_expr();
    ExprPtr comparison();
    ExprPtr bitwise_or();
    ExprPtr bitwise_xor();
    ExprPtr bitwise_and();
    ExprPtr shift();
    ExprPtr range_expr();
    ExprPtr concat();
    ExprPtr additive();
    ExprPtr multiplicative();
    ExprPtr power();
    ExprPtr unary();
    ExprPtr postfix(ExprPtr left);
    ExprPtr call_expr(ExprPtr callee);
    ExprPtr primary();
    ExprPtr fn_expr();

    std::vector<StmtPtr> func_body(); // reads body until 'end'
};

} // namespace pocketpp
