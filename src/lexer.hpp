#pragma once
#include "token.hpp"
#include <string>
#include <vector>

namespace pocketpp {

class Lexer {
public:
    explicit Lexer(std::string src) : src_(std::move(src)) {}
    std::vector<Token> tokenize();

private:
    std::string src_;
    size_t pos_{0};
    int line_{1};

    bool at_end() const { return pos_ >= src_.size(); }
    char cur() const { return at_end() ? '\0' : src_[pos_]; }
    char peek(int off=1) const {
        size_t p = pos_+off;
        return p < src_.size() ? src_[p] : '\0';
    }
    char advance() { char c = cur(); ++pos_; if(c=='\n') ++line_; return c; }
    bool match(char c) { if(!at_end() && src_[pos_]==c){ advance(); return true; } return false; }

    void skip_whitespace_and_comments(std::vector<Token>& out);
    Token make(TT t, std::string lex="") const { return Token{t, std::move(lex), 0, line_}; }
    Token make_num(double v, std::string lex) const { return Token{TT::NUMBER, std::move(lex), v, line_}; }

    Token read_string(char quote);
    Token read_number();
    // Returns multiple tokens for interpolated strings
    std::vector<Token> read_interp_string(char quote);
    std::vector<Token> scan_one();
};

} // namespace pocketpp
