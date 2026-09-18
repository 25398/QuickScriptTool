#include "var_compute.h"

#include "utils.h"

#include <cmath>
#include <cwctype>
#include <memory>
#include <unordered_set>
#include <vector>

namespace {

constexpr size_t kMaxSource = 16384;
constexpr int kMaxOps = 100000;
constexpr int kMaxLoopIters = 10000;
constexpr size_t kMaxLocals = 256;
constexpr size_t kMaxString = 65536;
constexpr size_t kMaxArray = 4096;

enum class TokKind {
    Eof, Number, String, Ident, KwInt, KwDouble, KwString, KwIf, KwElse,
    KwFor, KwWhile, KwReturn, Plus, Minus, Star, Slash, Percent,
    PlusPlus, MinusMinus, PlusEq, MinusEq, StarEq, SlashEq,
    Eq, EqEq, Bang, BangEq, Lt, LtEq, Gt, GtEq, AmpAmp, PipePipe,
    LParen, RParen, LBrace, RBrace, LBracket, RBracket, Semicolon, Comma, Dot
};

struct Token {
    TokKind kind = TokKind::Eof;
    std::wstring text;
    double number = 0;
};

struct Value {
    bool isString = false;
    bool isArray = false;
    double number = 0;
    std::wstring str;
    std::vector<Value> items;

    static Value Num(double n) {
        Value v;
        v.number = n;
        return v;
    }
    static Value Str(std::wstring s) {
        Value v;
        v.isString = true;
        v.str = std::move(s);
        return v;
    }
    static Value Arr(std::vector<Value> items) {
        Value v;
        v.isArray = true;
        v.items = std::move(items);
        return v;
    }
    bool IsNumber() const { return !isString && !isArray; }
    bool Truth() const {
        if (isArray) return !items.empty();
        if (isString) return !str.empty();
        return number != 0.0;
    }
};

bool IsLenProp(const std::wstring& prop) {
    return prop == L"count" || prop == L"length" || prop == L"size";
}

std::wstring TrimWs(const std::wstring& s) {
    size_t a = 0;
    while (a < s.size() && iswspace(s[a])) ++a;
    size_t b = s.size();
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::wstring FormatExport(const Value& v) {
    if (v.isArray) {
        std::wstring out;
        for (size_t i = 0; i < v.items.size(); ++i) {
            if (i) out += L",";
            out += FormatExport(v.items[i]);
        }
        return out;
    }
    if (v.isString) return v.str;
    if (std::isfinite(v.number) && std::abs(v.number - std::round(v.number)) < 1e-9
        && v.number >= -2147483647.0 && v.number <= 2147483647.0) {
        return std::to_wstring(static_cast<int>(std::round(v.number)));
    }
    return std::to_wstring(v.number);
}

bool IsIdentStart(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_';
}
bool IsIdentChar(wchar_t c) {
    return IsIdentStart(c) || (c >= L'0' && c <= L'9');
}

struct Lexer {
    const std::wstring& s;
    size_t i = 0;
    std::wstring err;

    bool lastSkipHadNl = false;

    explicit Lexer(const std::wstring& src) : s(src) {}

    void Skip() {
        lastSkipHadNl = false;
        while (i < s.size()) {
            const wchar_t c = s[i];
            if (c == L'\n') { lastSkipHadNl = true; ++i; continue; }
            if (c == L'\r') {
                lastSkipHadNl = true;
                ++i;
                if (i < s.size() && s[i] == L'\n') ++i;
                continue;
            }
            if (iswspace(c)) { ++i; continue; }
            if (c == L'/' && i + 1 < s.size() && s[i + 1] == L'/') {
                i += 2;
                while (i < s.size() && s[i] != L'\n') ++i;
                continue;
            }
            if (c == L'/' && i + 1 < s.size() && s[i + 1] == L'*') {
                i += 2;
                while (i + 1 < s.size() && !(s[i] == L'*' && s[i + 1] == L'/')) {
                    if (s[i] == L'\n' || s[i] == L'\r') lastSkipHadNl = true;
                    ++i;
                }
                if (i + 1 < s.size()) i += 2;
                continue;
            }
            break;
        }
    }

    Token LexQuoted(wchar_t close) {
        Token t;
        ++i;
        std::wstring out;
        while (i < s.size() && s[i] != close) {
            if (s[i] == L'\\' && i + 1 < s.size()) {
                const wchar_t e = s[i + 1];
                if (e == L'n') out.push_back(L'\n');
                else if (e == L't') out.push_back(L'\t');
                else if (e == L'r') out.push_back(L'\r');
                else if (e == L'\\') out.push_back(L'\\');
                else if (e == L'"') out.push_back(L'"');
                else if (e == L'\'') out.push_back(L'\'');
                else out.push_back(e);
                i += 2;
            } else {
                out.push_back(s[i++]);
            }
            if (out.size() > kMaxString) {
                err = L"字符串过长";
                t.kind = TokKind::Eof;
                return t;
            }
        }
        if (i >= s.size()) { err = L"字符串未闭合"; t.kind = TokKind::Eof; return t; }
        ++i;
        t.kind = TokKind::String;
        t.text = std::move(out);
        return t;
    }

    Token Next() {
        Skip();
        Token t;
        if (i >= s.size()) { t.kind = TokKind::Eof; return t; }
        const wchar_t c = s[i];
        if (c == L'"' || c == L'\'' || c == L'\u201C' || c == L'\u2018') {
            wchar_t close = c;
            if (c == L'\u201C') close = L'\u201D';
            else if (c == L'\u2018') close = L'\u2019';
            return LexQuoted(close);
        }
        if ((c >= L'0' && c <= L'9') || (c == L'.' && i + 1 < s.size() && s[i + 1] >= L'0' && s[i + 1] <= L'9')) {
            size_t start = i;
            while (i < s.size() && ((s[i] >= L'0' && s[i] <= L'9') || s[i] == L'.')) ++i;
            t.kind = TokKind::Number;
            t.text = s.substr(start, i - start);
            wchar_t* end = nullptr;
            t.number = wcstod(t.text.c_str(), &end);
            return t;
        }
        if (IsIdentStart(c)) {
            size_t start = i;
            ++i;
            while (i < s.size() && IsIdentChar(s[i])) ++i;
            if (s.compare(start, i - start, L"ctrl") == 0 && i < s.size() && s[i] == L':') {
                size_t j = i + 1;
                while (j < s.size() && IsIdentChar(s[j])) ++j;
                if (j < s.size() && s[j] == L'(' && j + 1 < s.size() && s[j + 1] == L')') {
                    i = j + 2;
                    t.kind = TokKind::Ident;
                    t.text = s.substr(start, i - start);
                    return t;
                }
            }
            t.text = s.substr(start, i - start);
            if (t.text == L"int") t.kind = TokKind::KwInt;
            else if (t.text == L"double") t.kind = TokKind::KwDouble;
            else if (t.text == L"string") t.kind = TokKind::KwString;
            else if (t.text == L"if") t.kind = TokKind::KwIf;
            else if (t.text == L"else") t.kind = TokKind::KwElse;
            else if (t.text == L"for") t.kind = TokKind::KwFor;
            else if (t.text == L"while") t.kind = TokKind::KwWhile;
            else if (t.text == L"return") t.kind = TokKind::KwReturn;
            else if (t.text == L"true") { t.kind = TokKind::Number; t.number = 1; }
            else if (t.text == L"false") { t.kind = TokKind::Number; t.number = 0; }
            else t.kind = TokKind::Ident;
            return t;
        }
        auto two = [&](wchar_t b, TokKind k2, TokKind k1) {
            if (i + 1 < s.size() && s[i + 1] == b) { i += 2; t.kind = k2; return true; }
            ++i; t.kind = k1; return true;
        };
        switch (c) {
        case L'+':
            if (i + 1 < s.size() && s[i + 1] == L'+') { i += 2; t.kind = TokKind::PlusPlus; break; }
            if (i + 1 < s.size() && s[i + 1] == L'=') { i += 2; t.kind = TokKind::PlusEq; break; }
            ++i; t.kind = TokKind::Plus; break;
        case L'-':
            if (i + 1 < s.size() && s[i + 1] == L'-') { i += 2; t.kind = TokKind::MinusMinus; break; }
            if (i + 1 < s.size() && s[i + 1] == L'=') { i += 2; t.kind = TokKind::MinusEq; break; }
            ++i; t.kind = TokKind::Minus; break;
        case L'*':
            if (i + 1 < s.size() && s[i + 1] == L'=') { i += 2; t.kind = TokKind::StarEq; break; }
            ++i; t.kind = TokKind::Star; break;
        case L'/':
            if (i + 1 < s.size() && s[i + 1] == L'=') { i += 2; t.kind = TokKind::SlashEq; break; }
            ++i; t.kind = TokKind::Slash; break;
        case L'%': ++i; t.kind = TokKind::Percent; break;
        case L'=': two(L'=', TokKind::EqEq, TokKind::Eq); break;
        case L'!': two(L'=', TokKind::BangEq, TokKind::Bang); break;
        case L'<': two(L'=', TokKind::LtEq, TokKind::Lt); break;
        case L'>': two(L'=', TokKind::GtEq, TokKind::Gt); break;
        case L'&':
            if (i + 1 < s.size() && s[i + 1] == L'&') { i += 2; t.kind = TokKind::AmpAmp; break; }
            err = L"不支持单 &"; t.kind = TokKind::Eof; break;
        case L'|':
            if (i + 1 < s.size() && s[i + 1] == L'|') { i += 2; t.kind = TokKind::PipePipe; break; }
            err = L"不支持单 |"; t.kind = TokKind::Eof; break;
        case L'(': ++i; t.kind = TokKind::LParen; break;
        case L')': ++i; t.kind = TokKind::RParen; break;
        case L'{': ++i; t.kind = TokKind::LBrace; break;
        case L'}': ++i; t.kind = TokKind::RBrace; break;
        case L'[': ++i; t.kind = TokKind::LBracket; break;
        case L']': ++i; t.kind = TokKind::RBracket; break;
        case L';': ++i; t.kind = TokKind::Semicolon; break;
        case L',': ++i; t.kind = TokKind::Comma; break;
        case L'.': ++i; t.kind = TokKind::Dot; break;
        default:
            err = std::wstring(L"非法字符: ") + c;
            t.kind = TokKind::Eof;
            break;
        }
        return t;
    }
};

enum class ExprKind {
    Number, String, Ident, Unary, Binary, Postfix, Call, Index, Property
};

struct Expr {
    ExprKind kind = ExprKind::Number;
    TokKind op = TokKind::Eof;
    double number = 0;
    std::wstring text;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
    std::vector<std::unique_ptr<Expr>> args;
};

enum class StmtKind { Block, If, For, While, Decl, Assign, Expr, Return, Empty };

struct Stmt {
    StmtKind kind = StmtKind::Empty;
    TokKind assignOp = TokKind::Eq;
    bool typedDecl = false;
    std::wstring name;
    std::vector<std::wstring> returnNames;
    std::unique_ptr<Expr> expr;
    std::unique_ptr<Expr> cond;
    std::unique_ptr<Stmt> init;
    std::unique_ptr<Stmt> step;
    std::unique_ptr<Stmt> thenS;
    std::unique_ptr<Stmt> elseS;
    std::vector<std::unique_ptr<Stmt>> body;
};

struct Parser {
    Lexer lex;
    Token cur;
    std::wstring err;

    explicit Parser(const std::wstring& src) : lex(src) { Advance(); }

    void Advance() {
        cur = lex.Next();
        if (!lex.err.empty() && err.empty()) err = lex.err;
    }
    bool Check(TokKind k) const { return cur.kind == k; }
    bool Match(TokKind k) {
        if (!Check(k)) return false;
        Advance();
        return true;
    }
    bool Expect(TokKind k, const wchar_t* msg) {
        if (Match(k)) return true;
        if (err.empty()) err = msg;
        return false;
    }
    void ExpectStmtEnd() {
        if (Match(TokKind::Semicolon)) return;
        if (lex.lastSkipHadNl || Check(TokKind::RBrace) || Check(TokKind::Eof) || IsStmtStart()) return;
        if (err.empty()) err = L"缺少 ;";
    }
    bool IsStmtStart() const {
        switch (cur.kind) {
        case TokKind::KwInt:
        case TokKind::KwDouble:
        case TokKind::KwString:
        case TokKind::KwIf:
        case TokKind::KwFor:
        case TokKind::KwWhile:
        case TokKind::KwReturn:
        case TokKind::LBrace:
            return true;
        default:
            return false;
        }
    }

    std::unique_ptr<Expr> MakeUnary(TokKind op, std::unique_ptr<Expr> inner) {
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Unary;
        e->op = op;
        e->left = std::move(inner);
        return e;
    }
    std::unique_ptr<Expr> MakeBinary(TokKind op, std::unique_ptr<Expr> a, std::unique_ptr<Expr> b) {
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Binary;
        e->op = op;
        e->left = std::move(a);
        e->right = std::move(b);
        return e;
    }

    std::unique_ptr<Expr> ParsePrimary() {
        if (Check(TokKind::Number)) {
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::Number;
            e->number = cur.number;
            Advance();
            return e;
        }
        if (Check(TokKind::String)) {
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::String;
            e->text = cur.text;
            Advance();
            return e;
        }
        if (Check(TokKind::Ident)) {
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::Ident;
            e->text = cur.text;
            Advance();
            while (Match(TokKind::Dot)) {
                if (!Check(TokKind::Ident)) {
                    if (err.empty()) err = L"属性名无效";
                    break;
                }
                e->text += L".";
                e->text += cur.text;
                Advance();
            }
            if (Match(TokKind::LParen)) {
                if (e->text.find(L'.') != std::wstring::npos) {
                    if (err.empty()) err = L"不支持方法调用，请用 split(文本, 分隔符)";
                    return e;
                }
                e->kind = ExprKind::Call;
                if (!Check(TokKind::RParen)) {
                    e->args.push_back(ParseExpr());
                    while (Match(TokKind::Comma)) e->args.push_back(ParseExpr());
                }
                Expect(TokKind::RParen, L"缺少 )");
            }
            return e;
        }
        if (Match(TokKind::LParen)) {
            auto inner = ParseExpr();
            Expect(TokKind::RParen, L"缺少 )");
            return inner;
        }
        if (err.empty()) err = L"期望表达式";
        return std::make_unique<Expr>();
    }

    std::unique_ptr<Expr> ParseUnary() {
        if (Check(TokKind::Bang) || Check(TokKind::Minus) || Check(TokKind::Plus)
            || Check(TokKind::PlusPlus) || Check(TokKind::MinusMinus)) {
            const TokKind op = cur.kind;
            Advance();
            if ((op == TokKind::Plus || op == TokKind::Minus)
                && (Check(TokKind::RParen) || Check(TokKind::RBrace) || Check(TokKind::Semicolon)
                    || Check(TokKind::Eof) || Check(TokKind::Comma) || Check(TokKind::LBrace)
                    || IsStmtStart())) {
                if (err.empty()) {
                    err = (op == TokKind::Plus)
                        ? L"裸写 + 是加法运算符，比较加号请写成 \"+\" 或 '+'"
                        : L"裸写 - 是减法运算符，比较减号请写成 \"-\" 或 '-'";
                }
                return std::make_unique<Expr>();
            }
            return MakeUnary(op, ParseUnary());
        }
        auto e = ParsePrimary();
        while (Check(TokKind::LBracket)) {
            Advance();
            auto idx = std::make_unique<Expr>();
            idx->kind = ExprKind::Index;
            idx->left = std::move(e);
            idx->right = ParseExpr();
            Expect(TokKind::RBracket, L"缺少 ]");
            e = std::move(idx);
        }
        while (Match(TokKind::Dot)) {
            if (!Check(TokKind::Ident)) {
                if (err.empty()) err = L"属性名无效";
                break;
            }
            auto p = std::make_unique<Expr>();
            p->kind = ExprKind::Property;
            p->left = std::move(e);
            p->text = cur.text;
            Advance();
            e = std::move(p);
        }
        if (Check(TokKind::PlusPlus) || Check(TokKind::MinusMinus)) {
            auto p = std::make_unique<Expr>();
            p->kind = ExprKind::Postfix;
            p->op = cur.kind;
            p->left = std::move(e);
            Advance();
            return p;
        }
        return e;
    }

    std::unique_ptr<Expr> ParseMul() {
        auto e = ParseUnary();
        while (Check(TokKind::Star) || Check(TokKind::Slash) || Check(TokKind::Percent)) {
            const TokKind op = cur.kind;
            Advance();
            e = MakeBinary(op, std::move(e), ParseUnary());
        }
        return e;
    }
    std::unique_ptr<Expr> ParseAdd() {
        auto e = ParseMul();
        while (Check(TokKind::Plus) || Check(TokKind::Minus)) {
            const TokKind op = cur.kind;
            Advance();
            e = MakeBinary(op, std::move(e), ParseMul());
        }
        return e;
    }
    std::unique_ptr<Expr> ParseCmp() {
        auto e = ParseAdd();
        while (Check(TokKind::Lt) || Check(TokKind::LtEq) || Check(TokKind::Gt) || Check(TokKind::GtEq)
            || Check(TokKind::EqEq) || Check(TokKind::BangEq)) {
            const TokKind op = cur.kind;
            Advance();
            e = MakeBinary(op, std::move(e), ParseAdd());
        }
        return e;
    }
    std::unique_ptr<Expr> ParseAnd() {
        auto e = ParseCmp();
        while (Match(TokKind::AmpAmp)) e = MakeBinary(TokKind::AmpAmp, std::move(e), ParseCmp());
        return e;
    }
    std::unique_ptr<Expr> ParseExpr() {
        auto e = ParseAnd();
        while (Match(TokKind::PipePipe)) e = MakeBinary(TokKind::PipePipe, std::move(e), ParseAnd());
        return e;
    }

    bool IsTypeKw() const {
        return Check(TokKind::KwInt) || Check(TokKind::KwDouble) || Check(TokKind::KwString);
    }

    std::unique_ptr<Stmt> ParseBlock() {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Block;
        Expect(TokKind::LBrace, L"缺少 {");
        while (!Check(TokKind::RBrace) && !Check(TokKind::Eof) && err.empty()) {
            s->body.push_back(ParseStmt());
        }
        Expect(TokKind::RBrace, L"缺少 }");
        return s;
    }

    std::unique_ptr<Stmt> ParseAssignOrExpr() {
        if (Check(TokKind::Ident)) {
            const std::wstring name = cur.text;
            const size_t savedI = lex.i;
            const Token savedCur = cur;
            const std::wstring savedErr = lex.err;
            const bool savedNl = lex.lastSkipHadNl;
            Advance();
            if (Check(TokKind::Eq) || Check(TokKind::PlusEq) || Check(TokKind::MinusEq)
                || Check(TokKind::StarEq) || Check(TokKind::SlashEq)) {
                auto s = std::make_unique<Stmt>();
                s->kind = StmtKind::Assign;
                s->name = name;
                s->assignOp = cur.kind;
                Advance();
                s->expr = ParseExpr();
                ExpectStmtEnd();
                return s;
            }
            lex.i = savedI;
            cur = savedCur;
            lex.err = savedErr;
            lex.lastSkipHadNl = savedNl;
        }
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Expr;
        s->expr = ParseExpr();
        ExpectStmtEnd();
        return s;
    }

    std::unique_ptr<Stmt> ParseStmt() {
        if (!err.empty()) return std::make_unique<Stmt>();
        if (Match(TokKind::Semicolon)) {
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::Empty;
            return s;
        }
        if (Check(TokKind::LBrace)) return ParseBlock();
        if (IsTypeKw()) {
            Advance();
            if (!Check(TokKind::Ident)) {
                if (err.empty()) err = L"声明缺少变量名";
                return std::make_unique<Stmt>();
            }
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::Decl;
            s->typedDecl = true;
            s->name = cur.text;
            Advance();
            if (Match(TokKind::Eq)) s->expr = ParseExpr();
            ExpectStmtEnd();
            return s;
        }
        if (Match(TokKind::KwIf)) {
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::If;
            Expect(TokKind::LParen, L"if 缺少 (");
            s->cond = ParseExpr();
            Expect(TokKind::RParen, L"if 缺少 )");
            s->thenS = ParseStmt();
            if (Match(TokKind::KwElse)) s->elseS = ParseStmt();
            return s;
        }
        if (Match(TokKind::KwWhile)) {
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::While;
            Expect(TokKind::LParen, L"while 缺少 (");
            s->cond = ParseExpr();
            Expect(TokKind::RParen, L"while 缺少 )");
            s->thenS = ParseStmt();
            return s;
        }
        if (Match(TokKind::KwFor)) {
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::For;
            Expect(TokKind::LParen, L"for 缺少 (");
            if (Match(TokKind::Semicolon)) {
                s->init = std::make_unique<Stmt>();
                s->init->kind = StmtKind::Empty;
            } else if (IsTypeKw() || Check(TokKind::Ident)) {
                s->init = ParseStmt();
            } else {
                s->init = std::make_unique<Stmt>();
                s->init->kind = StmtKind::Empty;
                Expect(TokKind::Semicolon, L"for 缺少 ;");
            }
            if (!Check(TokKind::Semicolon)) s->cond = ParseExpr();
            Expect(TokKind::Semicolon, L"for 缺少 ;");
            if (!Check(TokKind::RParen)) {
                auto step = std::make_unique<Stmt>();
                if (Check(TokKind::Ident)) {
                    step->name = cur.text;
                    Advance();
                    if (Check(TokKind::Eq) || Check(TokKind::PlusEq) || Check(TokKind::MinusEq)
                        || Check(TokKind::StarEq) || Check(TokKind::SlashEq)) {
                        step->kind = StmtKind::Assign;
                        step->assignOp = cur.kind;
                        Advance();
                        step->expr = ParseExpr();
                    } else if (Check(TokKind::PlusPlus) || Check(TokKind::MinusMinus)) {
                        step->kind = StmtKind::Expr;
                        auto p = std::make_unique<Expr>();
                        p->kind = ExprKind::Postfix;
                        p->op = cur.kind;
                        auto id = std::make_unique<Expr>();
                        id->kind = ExprKind::Ident;
                        id->text = step->name;
                        p->left = std::move(id);
                        Advance();
                        step->expr = std::move(p);
                    } else {
                        if (err.empty()) err = L"for 第三段无效";
                    }
                } else {
                    step->kind = StmtKind::Expr;
                    step->expr = ParseExpr();
                }
                s->step = std::move(step);
            }
            Expect(TokKind::RParen, L"for 缺少 )");
            s->thenS = ParseStmt();
            return s;
        }
        if (Match(TokKind::KwReturn)) {
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::Return;
            if (!Check(TokKind::Semicolon)) {
                if (!Check(TokKind::Ident)) {
                    if (err.empty()) err = L"return 只能列出变量名";
                    return s;
                }
                s->returnNames.push_back(cur.text);
                Advance();
                while (Match(TokKind::Comma)) {
                    if (!Check(TokKind::Ident)) {
                        if (err.empty()) err = L"return 只能列出变量名";
                        break;
                    }
                    s->returnNames.push_back(cur.text);
                    Advance();
                }
            }
            ExpectStmtEnd();
            return s;
        }
        return ParseAssignOrExpr();
    }

    std::vector<std::unique_ptr<Stmt>> ParseProgram() {
        std::vector<std::unique_ptr<Stmt>> out;
        while (!Check(TokKind::Eof) && err.empty()) out.push_back(ParseStmt());
        return out;
    }
};

struct Engine {
    const MacroVariableContext& ctx;
    const std::atomic<bool>* stopFlag = nullptr;
    std::unordered_map<std::wstring, Value> locals;
    std::wstring err;
    int ops = 0;
    bool returned = false;
    std::unordered_map<std::wstring, std::wstring> exported;

    bool Tick() {
        if (++ops > kMaxOps) { err = L"运算步数超限"; return false; }
        if (stopFlag && stopFlag->load(std::memory_order_relaxed)) {
            err = L"已停止";
            return false;
        }
        return err.empty();
    }

    bool ValidName(const std::wstring& name) const {
        if (name.empty() || !IsIdentStart(name[0])) return false;
        for (wchar_t c : name) if (!IsIdentChar(c)) return false;
        return true;
    }

    Value FromContext(const std::wstring& name) {
        if (name == L"ctrl:Clipboard()") {
            return Value::Str(ResolveClipboardVarCompute(ctx));
        }
        const std::wstring raw = ResolveMacroOperand(name, ctx);
        if (raw.empty()) return Value::Num(0);
        double n = 0;
        wchar_t* end = nullptr;
        n = wcstod(raw.c_str(), &end);
        if (end != raw.c_str() && *end == L'\0') return Value::Num(n);
        return Value::Str(raw);
    }

    Value* LocalPtr(const std::wstring& name) {
        const auto it = locals.find(name);
        if (it == locals.end()) return nullptr;
        return &it->second;
    }

    Value LoadIdent(const std::wstring& name) {
        if (auto* p = LocalPtr(name)) return *p;
        const size_t dot = name.find(L'.');
        if (dot != std::wstring::npos) {
            const std::wstring base = name.substr(0, dot);
            const std::wstring prop = name.substr(dot + 1);
            if (auto* p = LocalPtr(base)) {
                if (IsLenProp(prop)) {
                    if (p->isArray) return Value::Num(static_cast<double>(p->items.size()));
                    if (p->isString) return Value::Num(static_cast<double>(p->str.size()));
                }
            }
        }
        return FromContext(name);
    }

    bool StoreIdent(const std::wstring& name, const Value& v) {
        if (name.find(L'.') != std::wstring::npos) {
            err = L"不能给属性赋值（如 matchData）";
            return false;
        }
        if (!ValidName(name)) { err = L"非法变量名"; return false; }
        if (name.rfind(L"ctrl:", 0) == 0) { err = L"不能给内置变量赋值"; return false; }
        if (locals.find(name) == locals.end() && locals.size() >= kMaxLocals) {
            err = L"局部变量过多";
            return false;
        }
        if (v.isString && v.str.size() > kMaxString) { err = L"字符串过长"; return false; }
        if (v.isArray && v.items.size() > kMaxArray) { err = L"拆分结果过多"; return false; }
        locals[name] = v;
        return true;
    }

    Value ApplyAssign(TokKind op, const Value& lhs, const Value& rhs) {
        if (op == TokKind::Eq) return rhs;
        if (lhs.isArray || rhs.isArray) {
            err = L"数组不支持该运算";
            return {};
        }
        if (lhs.isString || rhs.isString) {
            if (op == TokKind::PlusEq) return Value::Str(FormatExport(lhs) + FormatExport(rhs));
            err = L"字符串不支持该运算";
            return {};
        }
        if (op == TokKind::PlusEq) return Value::Num(lhs.number + rhs.number);
        if (op == TokKind::MinusEq) return Value::Num(lhs.number - rhs.number);
        if (op == TokKind::StarEq) return Value::Num(lhs.number * rhs.number);
        if (op == TokKind::SlashEq) {
            if (rhs.number == 0) { err = L"除以 0"; return {}; }
            return Value::Num(lhs.number / rhs.number);
        }
        err = L"无效赋值运算";
        return {};
    }

    bool AsIndex(const Value& v, int& out, size_t n) {
        if (!v.IsNumber()) { err = L"下标必须是整数"; return false; }
        if (!std::isfinite(v.number) || std::abs(v.number - std::round(v.number)) >= 1e-9) {
            err = L"下标必须是整数";
            return false;
        }
        long long i = std::llround(v.number);
        if (i < 0) i += static_cast<long long>(n);
        if (i < 0 || static_cast<size_t>(i) >= n) { err = L"下标越界"; return false; }
        out = static_cast<int>(i);
        return true;
    }

    Value IndexValue(const Value& coll, const Value& idx) {
        if (coll.isArray) {
            int i = 0;
            if (!AsIndex(idx, i, coll.items.size())) return {};
            return coll.items[static_cast<size_t>(i)];
        }
        if (coll.isString) {
            int i = 0;
            if (!AsIndex(idx, i, coll.str.size())) return {};
            return Value::Str(std::wstring(1, coll.str[static_cast<size_t>(i)]));
        }
        err = L"只能对字符串或 split 结果取下标";
        return {};
    }

    Value LenOf(const Value& v) {
        if (v.isArray) return Value::Num(static_cast<double>(v.items.size()));
        if (v.isString) return Value::Num(static_cast<double>(v.str.size()));
        err = L"数字没有 count/length";
        return {};
    }

    Value ParseNumberText(const std::wstring& raw, bool asInt) {
        const std::wstring t = TrimWs(raw);
        if (t.empty()) { err = L"空字符串无法转为数字"; return {}; }
        wchar_t* end = nullptr;
        const double n = wcstod(t.c_str(), &end);
        if (end == t.c_str() || *end != L'\0') {
            err = L"无法转为数字: " + t;
            return {};
        }
        if (!std::isfinite(n)) { err = L"无法转为数字"; return {}; }
        if (asInt) return Value::Num(n >= 0 ? std::floor(n) : std::ceil(n));
        return Value::Num(n);
    }

    Value BuiltinSplit(const std::vector<Value>& args) {
        if (args.size() != 2 && args.size() != 3) {
            err = L"split 需要 2 或 3 个参数：split(文本, 分隔符) 或 split(文本, 分隔符, 最多段数)";
            return {};
        }
        if (args[0].isArray || args[1].isArray) {
            err = L"split 的参数不能是数组";
            return {};
        }
        const std::wstring s = FormatExport(args[0]);
        const std::wstring sep = FormatExport(args[1]);
        int maxParts = -1;
        if (args.size() == 3) {
            if (!args[2].IsNumber()) { err = L"split 第 3 个参数必须是数字（最多段数）"; return {}; }
            if (!std::isfinite(args[2].number)
                || std::abs(args[2].number - std::round(args[2].number)) >= 1e-9) {
                err = L"split 最多段数必须是整数";
                return {};
            }
            maxParts = static_cast<int>(std::round(args[2].number));
        }
        std::vector<Value> items;
        auto push = [&](std::wstring part) -> bool {
            if (items.size() >= kMaxArray) { err = L"拆分结果过多"; return false; }
            if (part.size() > kMaxString) { err = L"字符串过长"; return false; }
            items.push_back(Value::Str(std::move(part)));
            return true;
        };
        if (maxParts == 0) {
            if (!push(s)) return {};
            return Value::Arr(std::move(items));
        }
        if (sep.empty()) {
            if (s.empty()) return Value::Arr({});
            if (maxParts > 0 && static_cast<size_t>(maxParts) < s.size()) {
                for (int i = 0; i + 1 < maxParts; ++i) {
                    if (!push(std::wstring(1, s[static_cast<size_t>(i)]))) return {};
                }
                if (!push(s.substr(static_cast<size_t>(maxParts - 1)))) return {};
            } else {
                for (wchar_t c : s) {
                    if (!push(std::wstring(1, c))) return {};
                }
            }
            return Value::Arr(std::move(items));
        }
        size_t start = 0;
        while (true) {
            if (maxParts > 0 && static_cast<int>(items.size()) + 1 >= maxParts) {
                if (!push(s.substr(start))) return {};
                break;
            }
            const size_t pos = s.find(sep, start);
            if (pos == std::wstring::npos) {
                if (!push(s.substr(start))) return {};
                break;
            }
            if (!push(s.substr(start, pos - start))) return {};
            start = pos + sep.size();
        }
        return Value::Arr(std::move(items));
    }

    Value CallBuiltin(const std::wstring& name, const std::vector<Value>& args) {
        if (name == L"split") return BuiltinSplit(args);
        if (name == L"toInt" || name == L"toDouble" || name == L"toNumber") {
            if (args.size() != 1) { err = L"toInt/toNumber 需要 1 个参数"; return {}; }
            if (args[0].isArray) { err = L"不能把数组转为数字"; return {}; }
            if (args[0].IsNumber()) {
                if (name == L"toInt") {
                    const double n = args[0].number;
                    return Value::Num(n >= 0 ? std::floor(n) : std::ceil(n));
                }
                return args[0];
            }
            return ParseNumberText(FormatExport(args[0]), name == L"toInt");
        }
        if (name == L"toString" || name == L"str") {
            if (args.size() != 1) { err = L"toString 需要 1 个参数"; return {}; }
            return Value::Str(FormatExport(args[0]));
        }
        if (name == L"trim") {
            if (args.size() != 1) { err = L"trim 需要 1 个参数"; return {}; }
            if (args[0].isArray) { err = L"不能 trim 数组"; return {}; }
            return Value::Str(TrimWs(FormatExport(args[0])));
        }
        err = L"未知函数: " + name + L"（可用 split / toInt / toString / trim）";
        return {};
    }

    Value Eval(const Expr* e) {
        if (!e || !Tick()) return {};
        switch (e->kind) {
        case ExprKind::Number: return Value::Num(e->number);
        case ExprKind::String: return Value::Str(e->text);
        case ExprKind::Ident: return LoadIdent(e->text);
        case ExprKind::Call: {
            if (e->args.size() > 8) { err = L"函数参数过多"; return {}; }
            std::vector<Value> args;
            args.reserve(e->args.size());
            for (const auto& a : e->args) {
                args.push_back(Eval(a.get()));
                if (!err.empty()) return {};
            }
            return CallBuiltin(e->text, args);
        }
        case ExprKind::Index: {
            Value coll = Eval(e->left.get());
            Value idx = Eval(e->right.get());
            if (!err.empty()) return {};
            return IndexValue(coll, idx);
        }
        case ExprKind::Property: {
            Value v = Eval(e->left.get());
            if (!err.empty()) return {};
            if (!IsLenProp(e->text)) {
                err = L"未知属性: " + e->text + L"（可用 count / length）";
                return {};
            }
            return LenOf(v);
        }
        case ExprKind::Unary: {
            if (e->op == TokKind::PlusPlus || e->op == TokKind::MinusMinus) {
                if (!e->left || e->left->kind != ExprKind::Ident) {
                    err = L"++/-- 只能用于变量";
                    return {};
                }
                Value cur = LoadIdent(e->left->text);
                if (cur.isString || cur.isArray) { err = L"字符串不能 ++/--"; return {}; }
                const double n = cur.number + (e->op == TokKind::PlusPlus ? 1.0 : -1.0);
                const Value nv = Value::Num(n);
                StoreIdent(e->left->text, nv);
                return nv;
            }
            Value v = Eval(e->left.get());
            if (!err.empty()) return {};
            if (e->op == TokKind::Bang) return Value::Num(v.Truth() ? 0.0 : 1.0);
            if (e->op == TokKind::Plus) {
                if (v.isString) { err = L"不能对字符串取正"; return {}; }
                if (v.isArray) { err = L"不能对数组取正"; return {}; }
                return v;
            }
            if (e->op == TokKind::Minus) {
                if (v.isString) { err = L"不能对字符串取负"; return {}; }
                if (v.isArray) { err = L"不能对数组取负"; return {}; }
                return Value::Num(-v.number);
            }
            return {};
        }
        case ExprKind::Postfix: {
            if (!e->left || e->left->kind != ExprKind::Ident) {
                err = L"++/-- 只能用于变量";
                return {};
            }
            Value cur = LoadIdent(e->left->text);
            if (cur.isString || cur.isArray) { err = L"字符串不能 ++/--"; return {}; }
            const double old = cur.number;
            const double n = old + (e->op == TokKind::PlusPlus ? 1.0 : -1.0);
            StoreIdent(e->left->text, Value::Num(n));
            return Value::Num(old);
        }
        case ExprKind::Binary: {
            if (e->op == TokKind::AmpAmp) {
                Value a = Eval(e->left.get());
                if (!err.empty() || !a.Truth()) return Value::Num(a.Truth() ? 1.0 : 0.0);
                Value b = Eval(e->right.get());
                return Value::Num(b.Truth() ? 1.0 : 0.0);
            }
            if (e->op == TokKind::PipePipe) {
                Value a = Eval(e->left.get());
                if (!err.empty() || a.Truth()) return Value::Num(a.Truth() ? 1.0 : 0.0);
                Value b = Eval(e->right.get());
                return Value::Num(b.Truth() ? 1.0 : 0.0);
            }
            Value a = Eval(e->left.get());
            Value b = Eval(e->right.get());
            if (!err.empty()) return {};
            if (a.isArray || b.isArray) { err = L"数组不支持该运算"; return {}; }
            auto cmp = [&]() -> int {
                if (!a.isString && !b.isString) {
                    if (a.number < b.number) return -1;
                    if (a.number > b.number) return 1;
                    return 0;
                }
                const std::wstring as = a.isString ? a.str : FormatExport(a);
                const std::wstring bs = b.isString ? b.str : FormatExport(b);
                return as.compare(bs);
            };
            switch (e->op) {
            case TokKind::Plus:
                if (a.isString || b.isString)
                    return Value::Str(FormatExport(a) + FormatExport(b));
                return Value::Num(a.number + b.number);
            case TokKind::Minus:
                if (a.isString || b.isString) { err = L"字符串不能相减"; return {}; }
                return Value::Num(a.number - b.number);
            case TokKind::Star:
                if (a.isString || b.isString) { err = L"字符串不能相乘"; return {}; }
                return Value::Num(a.number * b.number);
            case TokKind::Slash:
                if (a.isString || b.isString) { err = L"字符串不能相除"; return {}; }
                if (b.number == 0) { err = L"除以 0"; return {}; }
                return Value::Num(a.number / b.number);
            case TokKind::Percent:
                if (a.isString || b.isString) { err = L"字符串不能取余"; return {}; }
                if (b.number == 0) { err = L"除以 0"; return {}; }
                return Value::Num(std::fmod(a.number, b.number));
            case TokKind::EqEq: return Value::Num(cmp() == 0 ? 1.0 : 0.0);
            case TokKind::BangEq: return Value::Num(cmp() != 0 ? 1.0 : 0.0);
            case TokKind::Lt: return Value::Num(cmp() < 0 ? 1.0 : 0.0);
            case TokKind::LtEq: return Value::Num(cmp() <= 0 ? 1.0 : 0.0);
            case TokKind::Gt: return Value::Num(cmp() > 0 ? 1.0 : 0.0);
            case TokKind::GtEq: return Value::Num(cmp() >= 0 ? 1.0 : 0.0);
            default: return {};
            }
        }
        }
        return {};
    }

    bool Exec(const Stmt* s) {
        if (!s || returned || !err.empty()) return err.empty();
        if (!Tick()) return false;
        switch (s->kind) {
        case StmtKind::Empty:
            return true;
        case StmtKind::Block:
            for (const auto& c : s->body) {
                if (!Exec(c.get()) || returned) break;
            }
            return err.empty();
        case StmtKind::Expr:
            Eval(s->expr.get());
            return err.empty();
        case StmtKind::Decl:
        case StmtKind::Assign: {
            Value rhs = s->expr ? Eval(s->expr.get()) : Value::Num(0);
            if (!err.empty()) return false;
            if (s->kind == StmtKind::Assign && s->assignOp != TokKind::Eq) {
                rhs = ApplyAssign(s->assignOp, LoadIdent(s->name), rhs);
                if (!err.empty()) return false;
            }
            return StoreIdent(s->name, rhs);
        }
        case StmtKind::If: {
            const Value c = Eval(s->cond.get());
            if (!err.empty()) return false;
            if (c.Truth()) return Exec(s->thenS.get());
            if (s->elseS) return Exec(s->elseS.get());
            return true;
        }
        case StmtKind::While: {
            int n = 0;
            while (err.empty() && !returned) {
                if (++n > kMaxLoopIters) { err = L"循环次数超限"; return false; }
                const Value c = Eval(s->cond.get());
                if (!err.empty() || !c.Truth()) break;
                if (!Exec(s->thenS.get())) return false;
            }
            return err.empty();
        }
        case StmtKind::For: {
            if (s->init && !Exec(s->init.get())) return false;
            int n = 0;
            while (err.empty() && !returned) {
                if (++n > kMaxLoopIters) { err = L"循环次数超限"; return false; }
                if (s->cond) {
                    const Value c = Eval(s->cond.get());
                    if (!err.empty() || !c.Truth()) break;
                }
                if (s->thenS && !Exec(s->thenS.get())) return false;
                if (returned) break;
                if (s->step && !Exec(s->step.get())) return false;
            }
            return err.empty();
        }
        case StmtKind::Return:
            for (const auto& name : s->returnNames) {
                if (!ValidName(name)) { err = L"return 变量名非法"; return false; }
                exported[name] = FormatExport(LoadIdent(name));
            }
            returned = true;
            return true;
        }
        return true;
    }
};

bool RunParser(const std::wstring& source, Parser& p, std::wstring& error) {
    if (source.size() > kMaxSource) {
        error = L"源码过长（最多 16384 字）";
        return false;
    }
    p.ParseProgram();
    if (!p.err.empty()) {
        error = p.err;
        return false;
    }
    return true;
}

void CollectReturns(const Stmt* s, std::vector<std::wstring>& names,
    std::unordered_set<std::wstring>& seen) {
    if (!s) return;
    switch (s->kind) {
    case StmtKind::Block:
        for (const auto& c : s->body) CollectReturns(c.get(), names, seen);
        break;
    case StmtKind::If:
        CollectReturns(s->thenS.get(), names, seen);
        CollectReturns(s->elseS.get(), names, seen);
        break;
    case StmtKind::While:
        CollectReturns(s->thenS.get(), names, seen);
        break;
    case StmtKind::For:
        CollectReturns(s->init.get(), names, seen);
        CollectReturns(s->thenS.get(), names, seen);
        CollectReturns(s->step.get(), names, seen);
        break;
    case StmtKind::Return:
        for (const auto& n : s->returnNames) {
            if (n.empty()) continue;
            if (seen.insert(n).second) names.push_back(n);
        }
        break;
    default:
        break;
    }
}

void ScanReturnFallback(const std::wstring& source, std::vector<std::wstring>& names) {
    std::unordered_set<std::wstring> seen(names.begin(), names.end());
    for (size_t i = 0; i < source.size(); ) {
        if (i + 6 <= source.size() && source.compare(i, 6, L"return") == 0) {
            const bool boundL = (i == 0) || !IsIdentChar(source[i - 1]);
            const bool boundR = (i + 6 >= source.size()) || !IsIdentChar(source[i + 6]);
            if (boundL && boundR) {
                size_t j = i + 6;
                auto skipWs = [&]() {
                    while (j < source.size() && iswspace(source[j])) ++j;
                };
                auto readIdent = [&]() -> std::wstring {
                    if (j >= source.size() || !IsIdentStart(source[j])) return {};
                    const size_t k0 = j;
                    while (j < source.size() && IsIdentChar(source[j])) ++j;
                    return source.substr(k0, j - k0);
                };
                skipWs();
                std::wstring n = readIdent();
                while (!n.empty()) {
                    if (seen.insert(n).second) names.push_back(n);
                    skipWs();
                    if (j >= source.size() || source[j] != L',') break;
                    ++j;
                    skipWs();
                    n = readIdent();
                }
                i = j;
                continue;
            }
        }
        ++i;
    }
}

std::vector<std::wstring> CollectReturnNamesImpl(const std::wstring& source) {
    std::vector<std::wstring> names;
    Parser p(source);
    auto prog = p.ParseProgram();
    if (p.err.empty()) {
        std::unordered_set<std::wstring> seen;
        for (const auto& s : prog) CollectReturns(s.get(), names, seen);
        return names;
    }
    ScanReturnFallback(source, names);
    return names;
}

}  // namespace

bool ParseVarCompute(const std::wstring& source, std::wstring& error) {
    Parser p(source);
    return RunParser(source, p, error);
}

VarComputeResult RunVarCompute(const std::wstring& source, const MacroVariableContext& ctx,
    const std::atomic<bool>* stopFlag) {
    VarComputeResult out;
    Parser p(source);
    if (!RunParser(source, p, out.error)) return out;
    Parser p2(source);
    auto prog = p2.ParseProgram();
    if (!p2.err.empty()) {
        out.error = p2.err;
        return out;
    }
    Engine eng{ ctx, stopFlag };
    for (const auto& s : prog) {
        if (!eng.Exec(s.get()) || eng.returned) break;
    }
    if (!eng.err.empty()) {
        out.error = eng.err;
        return out;
    }
    out.ok = true;
    out.exported = std::move(eng.exported);
    return out;
}

std::vector<std::wstring> CollectVarComputeReturnNames(const std::wstring& source) {
    return CollectReturnNamesImpl(source);
}
