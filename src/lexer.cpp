#include "lexer.hpp"
#include "errors.hpp"
#include <cstdlib>
#include <sstream>
#include <unordered_map>

namespace pocketpp {

static bool is_alpha(char c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static bool is_digit(char c) { return c>='0'&&c<='9'; }
static bool is_alnum(char c) { return is_alpha(c)||is_digit(c); }

static const std::unordered_map<std::string,TT> KEYWORDS = {
    {"def",TT::DEF},{"end",TT::END},{"fn",TT::FN},{"return",TT::RETURN},
    {"if",TT::IF},{"elif",TT::ELIF},{"else",TT::ELSE},{"then",TT::THEN},
    {"while",TT::WHILE},{"for",TT::FOR},{"in",TT::IN},{"do",TT::DO},{"break",TT::BREAK},
    {"class",TT::CLASS},{"is",TT::IS},{"self",TT::SELF},{"super",TT::SUPER},
    {"import",TT::IMPORT},{"from",TT::FROM},{"as",TT::AS},{"yield",TT::YIELD},
    {"true",TT::KW_TRUE},{"false",TT::KW_FALSE},{"null",TT::KW_NULL},
    {"not",TT::NOT},{"and",TT::AND},{"or",TT::OR},
};

// Parse a string of digits in given base, returning value
static double parse_int_base(const std::string& s, int base) {
    unsigned long long v = 0;
    for (char c : s) {
        v *= base;
        if (c>='0'&&c<='9') v += c-'0';
        else if (c>='a'&&c<='f') v += 10+(c-'a');
        else if (c>='A'&&c<='F') v += 10+(c-'A');
    }
    return (double)v;
}

Token Lexer::read_number() {
    size_t start = pos_-1;
    int saved_line = line_;
    std::string raw;
    raw += src_[start];
    char first = src_[start];

    // Check for 0b, 0x, 0X, 0o prefixes
    if (first == '0' && !at_end()) {
        char nx = cur();
        if (nx=='b'||nx=='B') {
            advance(); // consume 'b'
            std::string bits;
            while (!at_end() && (cur()=='0'||cur()=='1')) bits += advance();
            double v = parse_int_base(bits, 2);
            return Token{TT::NUMBER, "", v, saved_line};
        }
        if (nx=='x'||nx=='X') {
            advance();
            std::string hex;
            while (!at_end() && (is_digit(cur())||(cur()>='a'&&cur()<='f')||(cur()>='A'&&cur()<='F')))
                hex += advance();
            double v = parse_int_base(hex, 16);
            return Token{TT::NUMBER, "", v, saved_line};
        }
    }

    // Decimal: possibly starts with '.' (e.g. .5)
    bool first_dot = (first == '.');
    if (first_dot) {
        // already consumed '.', now read digits
        while (!at_end() && is_digit(cur())) raw += advance();
    } else {
        while (!at_end() && is_digit(cur())) raw += advance();
        if (!at_end() && cur()=='.' && pos_+1<src_.size() && is_digit(src_[pos_+1])) {
            raw += advance(); // '.'
            while (!at_end() && is_digit(cur())) raw += advance();
        }
    }
    // Scientific notation
    if (!at_end() && (cur()=='e'||cur()=='E')) {
        raw += advance();
        if (!at_end() && (cur()=='+'||cur()=='-')) raw += advance();
        while (!at_end() && is_digit(cur())) raw += advance();
    }
    double v = std::stod(raw);
    return Token{TT::NUMBER, raw, v, saved_line};
}

// Read a plain string (no interpolation) with given quote char
// Handles escape sequences and line continuation
Token Lexer::read_string(char quote) {
    int saved_line = line_;
    std::string result;
    while (!at_end()) {
        char c = advance();
        if (c == quote) break;
        if (c == '\\') {
            if (at_end()) break;
            char esc = advance();
            switch (esc) {
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                case 'r': result += '\r'; break;
                case '\\': result += '\\'; break;
                case '\'': result += '\''; break;
                case '"': result += '"'; break;
                case '\n': /* line continuation: ignore both */ break;
                default: result += esc; break;
            }
        } else {
            result += c;
        }
    }
    return Token{TT::STRING, result, 0, saved_line};
}

// Tokenize a string with interpolation into a sequence of tokens:
// STRING parts and embedded expression tokens separated by PLUS
// We return the constituent tokens to be inserted into the stream.
// Simple approach: build a synthetic concat expression via the parser.
// We emit: STRING_PART, PLUS, [expr tokens...], PLUS, STRING_PART, ...
// But that's complex. Instead: we emit a special INTERP marker.
// Simplest approach: re-lex the interpolated string as a special case.
// We return a flat token list where interpolated sections are wrapped
// in synthetic LPAREN/RPAREN with a "str()" call implied at parse time.
//
// Actually: we handle this differently. We accumulate segments and
// produce a sequence of tokens representing a string concatenation
// expression. We use DOT_DOT_CONCAT as the concat operator.

std::vector<Token> Lexer::read_interp_string(char quote) {
    // Returns tokens making up the interpolated expression
    // e.g. "Hello $name!" → STRING("Hello ") .. str(name) .. STRING("!")
    // We produce tokens: [STRING] [DOTDOT_CONCAT] [LPAREN expr RPAREN] ...
    // Actually easier: produce all parts as a vector, let parser handle StringConcatExpr.
    // We produce: STRING "Hello " | INTERP_START | IDENT name | INTERP_END | STRING "!" 
    // But we don't have those token types... 
    // 
    // Best approach: produce a flat list and mark them with a special token.
    // We'll use a technique: emit the string as a series of alternating
    // STRING tokens and TOKEN_GROUPS (which are just normal tokens for the expr).
    // The parser will reassemble them.
    //
    // We use: a "run" array. Segments are: (is_expr, content_or_tokens)
    // We produce:
    //   For plain segments: Token{STRING, content}
    //   For expr $name: Token{IDENT, "name"}  (single token)
    //   For expr ${...}: the tokens of the expression
    // Then wrap the whole thing so parser builds a concat.
    //
    // To pass tokens inline to parser: emit a synthetic STRING_INTERP_BEGIN token,
    // then alternating STRING and expr sections, then STRING_INTERP_END.
    // The parser recognizes this sequence.
    //
    // Since we don't have those tokens, the simplest clean solution:
    // Lex the whole string here, build a vector<Token> that represents
    // the concatenation expression in terms of existing tokens.
    // 
    // We'll emit: LPAREN STRING DOTDOT_CONCAT expr DOTDOT_CONCAT STRING ... RPAREN
    // But we need to wrap the whole thing so the parser treats it as one unit.
    // Let's just emit the components and let the caller wrap in GroupingExpr.
    //
    // Actually the cleanest: emit a special INTERP token with the raw string content
    // and let the parser handle it by calling back into the lexer for the embedded parts.
    // But that's a big change.
    //
    // PRACTICAL approach: Do it all here. Parse interpolation segments,
    // lex the expression parts inline, and return ALL tokens.
    // We'll emit: LPAREN [first-string] DOTDOT_CONCAT [expr-tokens] DOTDOT_CONCAT [string] RPAREN

    int saved_line = line_;
    std::vector<Token> result;
    // We'll accumulate parts: (false, string_content) or (true, vector<Token> expr_tokens)
    struct Part {
        bool is_expr;
        std::string str_part;
        std::vector<Token> expr_tokens;
    };
    std::vector<Part> parts;

    std::string cur_str;
    while (!at_end()) {
        char c = src_[pos_];
        if (c == quote) { advance(); break; }
        if (c == '\\') {
            advance();
            if (at_end()) break;
            char esc = advance();
            switch (esc) {
                case 'n': cur_str += '\n'; break;
                case 't': cur_str += '\t'; break;
                case 'r': cur_str += '\r'; break;
                case '\\': cur_str += '\\'; break;
                case '\'': cur_str += '\''; break;
                case '"': cur_str += '"'; break;
                case '\n': break; // line continuation
                default: cur_str += esc; break;
            }
            continue;
        }
        if (c == '$') {
            advance(); // consume '$'
            // Flush current string part
            parts.push_back({false, cur_str, {}});
            cur_str.clear();

            if (!at_end() && cur() == '{') {
                // ${expr}
                advance(); // consume '{'
                // Collect tokens until matching '}'
                std::vector<Token> expr_toks;
                int depth = 1;
                size_t expr_start = pos_;
                // We need to lex expr tokens. Use a sub-lexer approach:
                // collect raw chars until matching '}', then lex them.
                std::string expr_src;
                while (!at_end() && depth > 0) {
                    char ec = advance();
                    if (ec == '{') { depth++; expr_src += ec; }
                    else if (ec == '}') { depth--; if(depth>0) expr_src += ec; }
                    else expr_src += ec;
                }
                // Lex expr_src
                Lexer sub(expr_src + "\n");
                auto sub_toks = sub.tokenize();
                // Remove trailing NEWLINE and EOF
                while (!sub_toks.empty() && (sub_toks.back().type==TT::NEWLINE||sub_toks.back().type==TT::EOFILE))
                    sub_toks.pop_back();
                parts.push_back({true, "", sub_toks});
            } else {
                // $name (simple variable)
                if (!at_end() && is_alpha(cur())) {
                    std::string ident;
                    while (!at_end() && is_alnum(cur())) ident += advance();
                    Token t{TT::IDENT, ident, 0, line_};
                    auto kit = KEYWORDS.find(ident);
                    if (kit != KEYWORDS.end()) t.type = kit->second;
                    parts.push_back({true, "", {t}});
                } else {
                    // Treat $ as literal
                    parts.push_back({false, "$", {}});
                }
            }
            continue;
        }
        cur_str += advance();
    }
    // Flush last string
    parts.push_back({false, cur_str, {}});

    // Now build token stream representing concatenation
    // We emit: LPAREN part1 DOTDOT_CONCAT part2 ... RPAREN
    // Each string part → STRING token
    // Each expr part → LPAREN expr_tokens RPAREN
    // If only one part and it's a string, just emit STRING
    // Remove empty string parts from start/end
    // Filter: remove empty string parts that are adjacent (they'd be no-ops)
    
    result.push_back(Token{TT::LPAREN, "(", 0, saved_line});
    bool first = true;
    auto emit_concat = [&]() {
        if (!first) result.push_back(Token{TT::DOT_DOT, "..", 0, saved_line});
        first = false;
    };
    
    for (auto& part : parts) {
        if (part.is_expr) {
            emit_concat();
            result.push_back(Token{TT::IDENT, "str", 0, saved_line});
            result.push_back(Token{TT::LPAREN, "(", 0, saved_line});
            result.insert(result.end(), part.expr_tokens.begin(), part.expr_tokens.end());
            result.push_back(Token{TT::RPAREN, ")", 0, saved_line});
        } else if (!part.str_part.empty()) {
            emit_concat();
            result.push_back(Token{TT::STRING, part.str_part, 0, saved_line});
        }
    }
    if (first) {
        // Empty string
        result.push_back(Token{TT::STRING, "", 0, saved_line});
    }
    result.push_back(Token{TT::RPAREN, ")", 0, saved_line});
    return result;
}

void Lexer::skip_whitespace_and_comments(std::vector<Token>& /*out*/) {
    while (!at_end()) {
        char c = cur();
        if (c==' '||c=='\t'||c=='\r') { advance(); }
        else if (c=='#'||c=='\0') {
            // line comment
            while (!at_end()&&cur()!='\n') advance();
        } else if (c=='/'&&peek()=='/'||c=='/'&&peek()=='-') {
            // not actually a comment in pocketlang; '/' is divide
            break;
        } else break;
    }
}

std::vector<Token> Lexer::scan_one() {
    std::vector<Token> dummy;
    skip_whitespace_and_comments(dummy);

    if (at_end()) return {Token{TT::EOFILE,"",0,line_}};

    char c = advance();
    std::vector<Token> res;

    switch(c) {
    case '\n': res.push_back(Token{TT::NEWLINE,"\n",0,line_}); break;
    case ';':  res.push_back(Token{TT::SEMICOLON,";",0,line_}); break;
    case ',':  res.push_back(Token{TT::COMMA,",",0,line_}); break;
    case ':':  res.push_back(Token{TT::COLON,":",0,line_}); break;
    case '(':  res.push_back(Token{TT::LPAREN,"(",0,line_}); break;
    case ')':  res.push_back(Token{TT::RPAREN,")",0,line_}); break;
    case '[':  res.push_back(Token{TT::LBRACKET,"[",0,line_}); break;
    case ']':  res.push_back(Token{TT::RBRACKET,"]",0,line_}); break;
    case '{':  res.push_back(Token{TT::LBRACE,"{",0,line_}); break;
    case '}':  res.push_back(Token{TT::RBRACE,"}",0,line_}); break;
    case '~':  res.push_back(Token{TT::TILDE,"~",0,line_}); break;
    case '+':
        if (match('=')) res.push_back(Token{TT::PLUS_EQ,"+=",0,line_});
        else res.push_back(Token{TT::PLUS,"+",0,line_});
        break;
    case '-':
        if (match('=')) res.push_back(Token{TT::MINUS_EQ,"-=",0,line_});
        else res.push_back(Token{TT::MINUS,"-",0,line_});
        break;
    case '*':
        if (match('*')) {
            if (match('=')) res.push_back(Token{TT::STAR_STAR_EQ,"**=",0,line_});
            else res.push_back(Token{TT::STAR_STAR,"**",0,line_});
        } else if (match('=')) res.push_back(Token{TT::STAR_EQ,"*=",0,line_});
        else res.push_back(Token{TT::STAR,"*",0,line_});
        break;
    case '/':
        if (match('=')) res.push_back(Token{TT::SLASH_EQ,"/=",0,line_});
        else res.push_back(Token{TT::SLASH,"/",0,line_});
        break;
    case '%':
        if (match('=')) res.push_back(Token{TT::PERCENT_EQ,"%=",0,line_});
        else res.push_back(Token{TT::PERCENT,"%",0,line_});
        break;
    case '|':
        if (match('=')) res.push_back(Token{TT::PIPE_EQ,"|=",0,line_});
        else res.push_back(Token{TT::PIPE,"|",0,line_});
        break;
    case '&':
        if (match('=')) res.push_back(Token{TT::AMP_EQ,"&=",0,line_});
        else res.push_back(Token{TT::AMP,"&",0,line_});
        break;
    case '^':
        if (match('=')) res.push_back(Token{TT::CARET_EQ,"^=",0,line_});
        else res.push_back(Token{TT::CARET,"^",0,line_});
        break;
    case '<':
        if (match('<')) {
            if (match('=')) res.push_back(Token{TT::LSHIFT_EQ,"<<=",0,line_});
            else res.push_back(Token{TT::LSHIFT,"<<",0,line_});
        } else if (match('=')) res.push_back(Token{TT::LT_EQ,"<=",0,line_});
        else res.push_back(Token{TT::LT,"<",0,line_});
        break;
    case '>':
        if (match('>')) {
            if (match('=')) res.push_back(Token{TT::RSHIFT_EQ,">>=",0,line_});
            else res.push_back(Token{TT::RSHIFT,">>",0,line_});
        } else if (match('=')) res.push_back(Token{TT::GT_EQ,">=",0,line_});
        else res.push_back(Token{TT::GT,">",0,line_});
        break;
    case '!':
        if (match('=')) res.push_back(Token{TT::BANG_EQ,"!=",0,line_});
        else res.push_back(Token{TT::BANG,"!",0,line_});
        break;
    case '=':
        if (match('=')) res.push_back(Token{TT::EQ_EQ,"==",0,line_});
        else res.push_back(Token{TT::ASSIGN,"=",0,line_});
        break;
    case '.':
        if (!at_end() && cur()=='.') {
            advance();
            res.push_back(Token{TT::DOT_DOT,"..",0,line_});
        } else if (!at_end() && is_digit(cur())) {
            // .5 style float — put back the '.' by adjusting pos
            // Actually we already advanced past '.', treat as start of number
            --pos_; // un-advance
            // now pos_ is at '.'
            // We need to re-call read_number but it expects to have already consumed first char
            // Let's just handle it here
            std::string num_str = ".";
            advance(); // re-consume '.'
            while (!at_end()&&is_digit(cur())) num_str += advance();
            if (!at_end()&&(cur()=='e'||cur()=='E')) {
                num_str += advance();
                if (!at_end()&&(cur()=='+'||cur()=='-')) num_str += advance();
                while (!at_end()&&is_digit(cur())) num_str += advance();
            }
            res.push_back(Token{TT::NUMBER, num_str, std::stod(num_str), line_});
        } else {
            res.push_back(Token{TT::DOT,".",0,line_});
        }
        break;
    case '"':
    case '\'': {
        // Check for interpolation (only " strings support $ interpolation in pocketlang,
        // but we'll support both for safety)
        // Peek to see if there's a $ in this string
        // Actually, always try interpolation for both quote types
        // We need to re-scan from current position to detect $ before consuming
        // Simple: always use read_interp_string and it handles plain strings too
        auto toks = read_interp_string(c);
        // If result is just LPAREN STRING RPAREN, simplify to STRING
        if (toks.size()==3 && toks[0].type==TT::LPAREN && toks[1].type==TT::STRING && toks[2].type==TT::RPAREN) {
            res.push_back(toks[1]);
        } else {
            res.insert(res.end(), toks.begin(), toks.end());
        }
        break;
    }
    default:
        if (is_digit(c)) {
            // put c back into src by adjusting pos
            --pos_;
            if (c=='\\') { throw LexError("Unexpected character"); }
            advance(); // re-consume c
            // Actually read_number expects pos_ to be right after first digit
            // Let's redo: pos_ is now past c again
            Token t = read_number();
            t.line = line_;
            res.push_back(t);
        } else if (is_alpha(c)) {
            std::string ident;
            ident += c;
            while (!at_end()&&is_alnum(cur())) ident += advance();
            auto kit = KEYWORDS.find(ident);
            if (kit != KEYWORDS.end())
                res.push_back(Token{kit->second, ident, 0, line_});
            else
                res.push_back(Token{TT::IDENT, ident, 0, line_});
        } else {
            throw LexError("[line " + std::to_string(line_) + "] Unexpected character: " + std::string(1,c));
        }
        break;
    }
    return res;
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;

    // Handle #! shebang line
    if (pos_+1 < src_.size() && src_[pos_]=='#' && src_[pos_+1]=='!') {
        while (!at_end() && cur()!='\n') advance();
    }

    bool last_was_newline = true; // suppress leading newlines
    bool last_was_newline_or_semi = true;

    while (!at_end()) {
        auto toks = scan_one();
        for (auto& t : toks) {
            if (t.type == TT::EOFILE) {
                // emit final newline if needed
                if (!tokens.empty() && tokens.back().type != TT::NEWLINE)
                    tokens.push_back(Token{TT::NEWLINE,"\n",0,t.line});
                tokens.push_back(t);
                return tokens;
            }
            if (t.type == TT::NEWLINE) {
                if (!tokens.empty() &&
                    tokens.back().type != TT::NEWLINE &&
                    tokens.back().type != TT::SEMICOLON) {
                    tokens.push_back(t);
                }
            } else if (t.type == TT::SEMICOLON) {
                // treat as newline
                if (!tokens.empty() &&
                    tokens.back().type != TT::NEWLINE &&
                    tokens.back().type != TT::SEMICOLON) {
                    tokens.push_back(Token{TT::NEWLINE,";",0,t.line});
                }
            } else {
                tokens.push_back(t);
            }
        }
    }
    if (!tokens.empty() && tokens.back().type != TT::NEWLINE)
        tokens.push_back(Token{TT::NEWLINE,"",0,line_});
    tokens.push_back(Token{TT::EOFILE,"",0,line_});
    return tokens;
}

} // namespace pocketpp
