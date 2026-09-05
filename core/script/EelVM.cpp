//
// EelVM.cpp — see EelVM.h.
//
#include "EelVM.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace viz { namespace eel {

// ---------------------------------------------------------------------------
//  VM
// ---------------------------------------------------------------------------
// reg00..reg99 — process-global registers shared by every VM instance
// (AVS semantics: any scripted effect can read what another wrote).
static double g_regs[100];

double* VM::globalRegSlot(int i)
{
    return &g_regs[std::min(99, std::max(0, i))];
}

double* VM::var(const std::string& name)
{
    std::string key;
    key.reserve(name.size());
    for (char ch : name) key.push_back((char)std::tolower((unsigned char)ch));
    if (key.size() == 5 && key.compare(0, 3, "reg") == 0 &&
        std::isdigit((unsigned char)key[3]) && std::isdigit((unsigned char)key[4]))
        return &g_regs[(key[3] - '0') * 10 + (key[4] - '0')];
    auto it = _vars.find(key);
    if (it != _vars.end()) return it->second;
    _slots.push_back(0.0);
    double* p = &_slots.back();
    _vars.emplace(std::move(key), p);
    return p;
}

// megabuf storage: sparse pages of 65536 doubles, index range [0, 8M)
// (NSEEL's classic 128 blocks x 65536 items).
static const int kBufPage = 65536;
static const double kBufMax = 128.0 * kBufPage;

static double* bufSlot(std::unordered_map<int, std::vector<double>>& pages,
                       double index)
{
    static double scratch;                 // out-of-range sink; reads as 0
    if (!(index >= 0.0) || index >= kBufMax) { scratch = 0.0; return &scratch; }
    int i = (int)index;
    auto& page = pages[i / kBufPage];
    if (page.empty()) page.assign(kBufPage, 0.0);
    return &page[i % kBufPage];
}

double* VM::megabufSlot(double index) { return bufSlot(_megapages, index); }

static std::unordered_map<int, std::vector<double>>& gmegaPages()
{
    static std::unordered_map<int, std::vector<double>> g_pages;
    return g_pages;
}

double* VM::gmegabufSlot(double index) { return bufSlot(gmegaPages(), index); }

void VM::resetShared()
{
    for (double& r : g_regs) r = 0.0;
    gmegaPages().clear();
}

double VM::rand01()
{
    uint32_t x = rngState;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rngState = x;
    return (x & 0xFFFFFF) / (double)0x1000000;
}

// ---------------------------------------------------------------------------
//  Function table (Op/Fn enums live in EelVM.h for tooling)
// ---------------------------------------------------------------------------
struct FnDef { const char* name; int fn; int args; };
static const FnDef kFns[] = {
    {"sin",FN_SIN,1},{"cos",FN_COS,1},{"tan",FN_TAN,1},
    {"asin",FN_ASIN,1},{"acos",FN_ACOS,1},{"atan",FN_ATAN,1},{"atan2",FN_ATAN2,2},
    {"sqr",FN_SQR,1},{"sqrt",FN_SQRT,1},{"pow",FN_POW,2},
    {"exp",FN_EXP,1},{"log",FN_LOG,1},{"log10",FN_LOG10,1},
    {"abs",FN_ABS,1},{"min",FN_MIN,2},{"max",FN_MAX,2},{"sign",FN_SIGN,1},
    {"rand",FN_RAND,1},{"floor",FN_FLOOR,1},{"ceil",FN_CEIL,1},
    {"invsqrt",FN_INVSQRT,1},{"sigmoid",FN_SIGMOID,2},
    {"band",FN_BAND,2},{"bor",FN_BOR,2},{"bnot",FN_BNOT,1},
    {"if",FN_IF,3},{"equal",FN_EQUAL,2},{"above",FN_ABOVE,2},{"below",FN_BELOW,2},
    {"exec2",FN_EXEC2,2},{"exec3",FN_EXEC3,3},
    {"getosc",FN_GETOSC,3},{"getspec",FN_GETSPEC,3},{"gettime",FN_GETTIME,1},
    {"megabuf",FN_MEGABUF,1},{"gmegabuf",FN_GMEGABUF,1},{"assign",FN_ASSIGN,2},
    // extensions beyond ns-eel1
    {"lerp",FN_LERP,3},{"clamp",FN_CLAMP,3},{"frac",FN_FRAC,1},
    {"smoothstep",FN_SMOOTHSTEP,3},{"noise",FN_NOISE,2},
    {"hsv",FN_HSV,3},{"getr",FN_GETR,1},{"getg",FN_GETG,1},{"getb",FN_GETB,1},
};

// ---------------------------------------------------------------------------
//  Tokenizer
// ---------------------------------------------------------------------------
namespace {
enum TokKind { T_END, T_NUM, T_IDENT, T_CONST, T_PUNCT };

struct Token {
    TokKind kind = T_END;
    double num = 0;
    std::string text;      // ident (lowercased) or punct
};

struct Lexer {
    const char* p;
    std::string err;

    explicit Lexer(const char* src) : p(src) {}

    void skipWs() {
        for (;;) {
            while (*p && (std::isspace((unsigned char)*p))) ++p;
            if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') ++p; continue; }
            if (p[0] == '/' && p[1] == '*') {
                p += 2;
                while (*p && !(p[0] == '*' && p[1] == '/')) ++p;
                if (*p) p += 2;
                continue;
            }
            break;
        }
    }

    Token next() {
        skipWs();
        Token t;
        if (!*p) { t.kind = T_END; return t; }
        unsigned char c = (unsigned char)*p;

        if (std::isdigit(c) || (c == '.' && std::isdigit((unsigned char)p[1]))) {
            char* end = nullptr;
            t.kind = T_NUM;
            t.num = std::strtod(p, &end);
            p = end;
            return t;
        }
        if (c == '$') {                       // $PI etc.
            ++p;
            std::string name;
            while (std::isalnum((unsigned char)*p)) name.push_back((char)std::tolower((unsigned char)*p++));
            t.kind = T_NUM;
            if      (name == "pi")  t.num = 3.141592653589793;
            else if (name == "e")   t.num = 2.718281828459045;
            else if (name == "phi") t.num = 1.618033988749895;
            else { err = "unknown constant $" + name; t.kind = T_END; }
            return t;
        }
        if (std::isalpha(c) || c == '_') {
            t.kind = T_IDENT;
            while (std::isalnum((unsigned char)*p) || *p == '_')
                t.text.push_back((char)std::tolower((unsigned char)*p++));
            return t;
        }
        // punct (multi-char first)
        static const char* two[] = {"<=", ">=", "==", "!="};
        for (const char* s : two) {
            if (p[0] == s[0] && p[1] == s[1]) { t.kind = T_PUNCT; t.text = s; p += 2; return t; }
        }
        t.kind = T_PUNCT;
        t.text.assign(1, (char)c);
        ++p;
        return t;
    }
};

// ---------------------------------------------------------------------------
//  Parser (recursive descent)
// ---------------------------------------------------------------------------
struct Parser {
    VM& vm;
    Lexer lex;
    Token cur;
    std::vector<Node>& nodes;
    std::string err;

    Parser(VM& v, const char* src, std::vector<Node>& n)
        : vm(v), lex(src), nodes(n) { advance(); }

    void advance() { cur = lex.next(); if (!lex.err.empty() && err.empty()) err = lex.err; }
    bool isP(const char* s) const { return cur.kind == T_PUNCT && cur.text == s; }
    bool eatP(const char* s) { if (isP(s)) { advance(); return true; } return false; }
    void fail(const std::string& m) { if (err.empty()) err = m; }

    int mk(int op, int a = -1, int b = -1, int c = -1) {
        Node n; n.op = op; n.a = a; n.b = b; n.c = c;
        nodes.push_back(n);
        return (int)nodes.size() - 1;
    }

    // statements: expr (';' expr)* with empty statements tolerated
    std::vector<int> parseStatements() {
        std::vector<int> roots;
        for (;;) {
            while (eatP(";")) {}
            if (cur.kind == T_END || !err.empty()) break;
            int e = parseAssign();
            if (!err.empty()) break;
            roots.push_back(e);
            if (cur.kind == T_END) break;
            if (!eatP(";")) { fail("expected ';' near '" + cur.text + "'"); break; }
        }
        return roots;
    }

    int parseAssign() {
        int lhs = parseTernary();
        if (!err.empty()) return lhs;
        if (isP("=")) {
            advance();
            if (nodes[lhs].op == OP_CALL &&
                (nodes[lhs].fn == FN_MEGABUF || nodes[lhs].fn == FN_GMEGABUF)) {
                int rhs = parseAssign();      // right-assoc
                int n = mk(OP_BUFASSIGN, nodes[lhs].a, rhs);
                nodes[n].fn = nodes[lhs].fn;
                return n;
            }
            if (nodes[lhs].op != OP_VAR) { fail("left side of '=' must be a variable"); return lhs; }
            int rhs = parseAssign();          // right-assoc
            int n = mk(OP_ASSIGN, -1, rhs);
            nodes[n].slot = nodes[lhs].slot;
            return n;
        }
        return lhs;
    }

    int parseTernary() {
        int c = parseBitOr();
        if (isP("?")) {
            advance();
            int t = parseAssign();
            if (!eatP(":")) { fail("expected ':' in ternary"); return c; }
            int f = parseAssign();
            return mk(OP_TERNARY, c, t, f);
        }
        return c;
    }

    int parseBitOr() {
        int l = parseBitAnd();
        while (isP("|")) { advance(); l = mk(OP_BITOR, l, parseBitAnd()); }
        return l;
    }
    int parseBitAnd() {
        int l = parseCmp();
        while (isP("&")) { advance(); l = mk(OP_BITAND, l, parseCmp()); }
        return l;
    }
    int parseCmp() {
        int l = parseAdd();
        for (;;) {
            int op = -1;
            if      (isP("<"))  op = OP_LT;
            else if (isP(">"))  op = OP_GT;
            else if (isP("<=")) op = OP_LE;
            else if (isP(">=")) op = OP_GE;
            else if (isP("==")) op = OP_EQ;
            else if (isP("!=")) op = OP_NE;
            else break;
            advance();
            l = mk(op, l, parseAdd());
        }
        return l;
    }
    int parseAdd() {
        int l = parseMul();
        for (;;) {
            if      (isP("+")) { advance(); l = mk(OP_ADD, l, parseMul()); }
            else if (isP("-")) { advance(); l = mk(OP_SUB, l, parseMul()); }
            else break;
        }
        return l;
    }
    int parseMul() {
        int l = parsePow();
        for (;;) {
            if      (isP("*")) { advance(); l = mk(OP_MUL, l, parsePow()); }
            else if (isP("/")) { advance(); l = mk(OP_DIV, l, parsePow()); }
            else if (isP("%")) { advance(); l = mk(OP_MOD, l, parsePow()); }
            else break;
        }
        return l;
    }
    int parsePow() {
        int l = parseUnary();
        if (isP("^")) { advance(); return mk(OP_POW, l, parsePow()); }   // right-assoc
        return l;
    }
    int parseUnary() {
        if (eatP("-")) return mk(OP_NEG, parseUnary());
        if (eatP("+")) return parseUnary();
        if (eatP("!")) {                       // logical not via bnot
            int a = parseUnary();
            int n = mk(OP_CALL, a);
            nodes[n].fn = FN_BNOT;
            return n;
        }
        return parsePrimary();
    }

    int parsePrimary() {
        if (cur.kind == T_NUM) {
            int n = mk(OP_NUM);
            nodes[n].num = cur.num;
            advance();
            return n;
        }
        if (eatP("(")) {
            int e = parseAssign();
            if (!eatP(")")) fail("expected ')'");
            return e;
        }
        if (cur.kind == T_IDENT) {
            std::string name = cur.text;
            advance();
            if (eatP("(")) return parseCall(name);
            int n = mk(OP_VAR);
            nodes[n].slot = vm.var(name);
            return n;
        }
        fail("unexpected token '" + cur.text + "'");
        return mk(OP_NUM);
    }

    int parseCall(const std::string& name) {
        const FnDef* def = nullptr;
        for (const auto& f : kFns)
            if (name == f.name) { def = &f; break; }
        if (!def) { fail("unknown function '" + name + "'"); return mk(OP_NUM); }

        int args[3] = {-1, -1, -1};
        int argc = 0;
        if (!isP(")")) {
            for (;;) {
                int e = parseAssign();
                if (argc < 3) args[argc] = e;
                argc++;
                if (!eatP(",")) break;
            }
        }
        if (!eatP(")")) fail("expected ')' after arguments to " + name);
        if (argc != def->args)
            fail(name + " expects " + std::to_string(def->args) + " argument(s)");
        int n = mk(OP_CALL, args[0], args[1], args[2]);
        nodes[n].fn = def->fn;
        return n;
    }
};
} // anonymous namespace

// ---------------------------------------------------------------------------
//  Compile
// ---------------------------------------------------------------------------
Program compile(VM& vm, const std::string& src)
{
    Program prog;
    prog._vm = &vm;
    Parser parser(vm, src.c_str(), prog._nodes);
    prog._stmts = parser.parseStatements();
    prog._error = parser.err;
    if (!prog._error.empty()) prog._stmts.clear();
    return prog;
}

// ---------------------------------------------------------------------------
//  Eval
// ---------------------------------------------------------------------------
static inline double truthy(double v) { return v != 0.0 ? 1.0 : 0.0; }

double Program::eval(int idx) const
{
    const Node& n = _nodes[idx];
    switch (n.op) {
        case OP_NUM:    return n.num;
        case OP_VAR:    return *n.slot;
        case OP_ASSIGN: { double v = eval(n.b); *n.slot = v; return v; }
        case OP_ADD:    return eval(n.a) + eval(n.b);
        case OP_SUB:    return eval(n.a) - eval(n.b);
        case OP_MUL:    return eval(n.a) * eval(n.b);
        case OP_DIV:    { double b = eval(n.b); return b == 0.0 ? 0.0 : eval(n.a) / b; }
        case OP_MOD:    { long long b = (long long)eval(n.b);
                          if (b == 0) return 0.0;
                          return (double)((long long)eval(n.a) % b); }
        case OP_POW:    return std::pow(eval(n.a), eval(n.b));
        case OP_NEG:    return -eval(n.a);
        case OP_LT:     return eval(n.a) <  eval(n.b) ? 1.0 : 0.0;
        case OP_GT:     return eval(n.a) >  eval(n.b) ? 1.0 : 0.0;
        case OP_LE:     return eval(n.a) <= eval(n.b) ? 1.0 : 0.0;
        case OP_GE:     return eval(n.a) >= eval(n.b) ? 1.0 : 0.0;
        case OP_EQ:     return eval(n.a) == eval(n.b) ? 1.0 : 0.0;
        case OP_NE:     return eval(n.a) != eval(n.b) ? 1.0 : 0.0;
        case OP_BITAND: return (double)((long long)eval(n.a) & (long long)eval(n.b));
        case OP_BITOR:  return (double)((long long)eval(n.a) | (long long)eval(n.b));
        case OP_TERNARY:return eval(n.a) != 0.0 ? eval(n.b) : eval(n.c);
        case OP_BUFASSIGN: {
            double idx = eval(n.a), v = eval(n.b);
            double* p = (n.fn == FN_MEGABUF) ? _vm->megabufSlot(idx)
                                             : eel::VM::gmegabufSlot(idx);
            *p = v;
            return v;
        }
        case OP_CALL:   break;   // below
        default:        return 0.0;
    }

    // OP_CALL
    switch (n.fn) {
        case FN_SIN:    return std::sin(eval(n.a));
        case FN_COS:    return std::cos(eval(n.a));
        case FN_TAN:    return std::tan(eval(n.a));
        case FN_ASIN:   { double v = eval(n.a); return v < -1 || v > 1 ? 0.0 : std::asin(v); }
        case FN_ACOS:   { double v = eval(n.a); return v < -1 || v > 1 ? 0.0 : std::acos(v); }
        case FN_ATAN:   return std::atan(eval(n.a));
        case FN_ATAN2:  return std::atan2(eval(n.a), eval(n.b));
        case FN_SQR:    { double v = eval(n.a); return v * v; }
        case FN_SQRT:   { double v = eval(n.a); return v <= 0 ? 0.0 : std::sqrt(v); }
        case FN_POW:    return std::pow(eval(n.a), eval(n.b));
        case FN_EXP:    return std::exp(eval(n.a));
        case FN_LOG:    { double v = eval(n.a); return v <= 0 ? 0.0 : std::log(v); }
        case FN_LOG10:  { double v = eval(n.a); return v <= 0 ? 0.0 : std::log10(v); }
        case FN_ABS:    return std::fabs(eval(n.a));
        case FN_MIN:    { double a = eval(n.a), b = eval(n.b); return a < b ? a : b; }
        case FN_MAX:    { double a = eval(n.a), b = eval(n.b); return a > b ? a : b; }
        case FN_SIGN:   { double v = eval(n.a); return v > 0 ? 1.0 : (v < 0 ? -1.0 : 0.0); }
        case FN_RAND:   { double m = eval(n.a);
                          if (m < 1.0) return _vm->rand01();
                          return std::floor(_vm->rand01() * m); }
        case FN_FLOOR:  return std::floor(eval(n.a));
        case FN_CEIL:   return std::ceil(eval(n.a));
        case FN_INVSQRT:{ double v = eval(n.a); return v <= 0 ? 0.0 : 1.0 / std::sqrt(v); }
        case FN_SIGMOID:{ double v = eval(n.a), c = eval(n.b);
                          double t = 1.0 + std::exp(-v * c);
                          return t != 0.0 ? 1.0 / t : 0.0; }
        case FN_BAND:   return truthy(eval(n.a)) * truthy(eval(n.b));
        case FN_BOR:    return (eval(n.a) != 0.0 || eval(n.b) != 0.0) ? 1.0 : 0.0;
        case FN_BNOT:   return eval(n.a) == 0.0 ? 1.0 : 0.0;
        case FN_IF:     { double c = eval(n.a), t = eval(n.b), f = eval(n.c);
                          return c != 0.0 ? t : f; }   // eager, like ns-eel1
        case FN_EQUAL:  return eval(n.a) == eval(n.b) ? 1.0 : 0.0;
        case FN_ABOVE:  return eval(n.a) >  eval(n.b) ? 1.0 : 0.0;
        case FN_BELOW:  return eval(n.a) <  eval(n.b) ? 1.0 : 0.0;
        case FN_EXEC2:  { eval(n.a); return eval(n.b); }
        case FN_EXEC3:  { eval(n.a); eval(n.b); return eval(n.c); }
        case FN_GETOSC: return _vm->getosc ? _vm->getosc(eval(n.a), eval(n.b), eval(n.c)) : 0.0;
        case FN_GETSPEC:return _vm->getspec ? _vm->getspec(eval(n.a), eval(n.b), eval(n.c)) : 0.0;
        case FN_GETTIME:return _vm->gettime ? _vm->gettime(eval(n.a)) : 0.0;
        case FN_LERP:   { double a = eval(n.a), b = eval(n.b); return a + (b - a) * eval(n.c); }
        case FN_CLAMP:  { double x = eval(n.a), lo = eval(n.b), hi = eval(n.c);
                          return x < lo ? lo : (x > hi ? hi : x); }
        case FN_FRAC:   { double v = eval(n.a); return v - std::floor(v); }
        case FN_SMOOTHSTEP: {
            double e0 = eval(n.a), e1 = eval(n.b), x = eval(n.c);
            if (e0 == e1) return x < e0 ? 0.0 : 1.0;
            double t = (x - e0) / (e1 - e0);
            t = t < 0 ? 0 : (t > 1 ? 1 : t);
            return t * t * (3.0 - 2.0 * t);
        }
        case FN_NOISE: {                     // value noise, 0..1, deterministic
            double x = eval(n.a), y = eval(n.b);
            auto hash = [](double ix, double iy) {
                double s = std::sin(ix * 127.1 + iy * 311.7) * 43758.5453;
                return s - std::floor(s);
            };
            double ix = std::floor(x), iy = std::floor(y);
            double fx = x - ix, fy = y - iy;
            double ux = fx * fx * (3.0 - 2.0 * fx), uy = fy * fy * (3.0 - 2.0 * fy);
            double a = hash(ix, iy),     b2 = hash(ix + 1, iy);
            double c = hash(ix, iy + 1), d = hash(ix + 1, iy + 1);
            return a + (b2 - a) * ux + (c - a) * uy + (a - b2 - c + d) * ux * uy;
        }
        case FN_HSV: {                       // packed 0xRRGGBB like AVS colors
            double h = eval(n.a), s = eval(n.b), v = eval(n.c);
            h -= std::floor(h);
            auto clamp01 = [](double x) { return x < 0 ? 0.0 : (x > 1 ? 1.0 : x); };
            double r = clamp01(std::fabs(h * 6.0 - 3.0) - 1.0);
            double g = clamp01(2.0 - std::fabs(h * 6.0 - 2.0));
            double b2 = clamp01(2.0 - std::fabs(h * 6.0 - 4.0));
            auto q = [&](double c) { return std::floor(clamp01(v * (1.0 + s * (c - 1.0))) * 255.0 + 0.5); };
            return q(r) * 65536.0 + q(g) * 256.0 + q(b2);
        }
        case FN_GETR:   return (double)(((long long)eval(n.a) >> 16) & 255) / 255.0;
        case FN_GETG:   return (double)(((long long)eval(n.a) >> 8) & 255) / 255.0;
        case FN_GETB:   return (double)((long long)eval(n.a) & 255) / 255.0;
        case FN_MEGABUF: return *_vm->megabufSlot(eval(n.a));
        case FN_GMEGABUF:return *eel::VM::gmegabufSlot(eval(n.a));
        case FN_ASSIGN: {                    // assign(var-or-buf, value)
            double v = eval(n.b);
            const Node& dst = _nodes[n.a];
            if (dst.op == OP_VAR) *dst.slot = v;
            else if (dst.op == OP_CALL && dst.fn == FN_MEGABUF)
                *_vm->megabufSlot(eval(dst.a)) = v;
            else if (dst.op == OP_CALL && dst.fn == FN_GMEGABUF)
                *eel::VM::gmegabufSlot(eval(dst.a)) = v;
            return v;
        }
        default:        return 0.0;
    }
}

void Program::run() const
{
    if (!ok()) return;
    for (int root : _stmts) eval(root);
}

}} // namespace viz::eel
