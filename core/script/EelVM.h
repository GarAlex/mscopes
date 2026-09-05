//
// EelVM.h — our own EEL-compatible expression engine.
//
// Implements the NS-EEL dialect that AVS preset scripts are written in
// (Superscope, Dynamic Movement, …): ';'-separated statements, '='
// assignment, auto-created case-insensitive double variables, arithmetic
// (+ - * / % ^), comparisons, if/above/below/equal/band/bor/bnot, the math
// builtins, rand(), $PI/$E/$PHI, and '//' + '/* */' comments.
//
// Design: tokenizer → recursive-descent parser → AST in a flat vector →
// recursive eval. No JIT — AVS scripts run per *point/grid cell*, not per
// pixel, so an interpreter comfortably holds 60fps. Portable C++17, no deps.
//
// Semantics follow ns-eel1 (the engine classic AVS used):
//   - everything is a double; variables auto-create as 0
//   - if(c,t,f) evaluates all args (no short-circuit)
//   - % is integer modulo (b==0 → 0); / by 0 → 0
//   - rand(x) is an integer in [0, x) for x>=1
//   - band/bor/bnot are logical (0/1); & | are integer bitwise
//
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace viz { namespace eel {

class VM {
public:
    // Get-or-create a variable slot. Pointers remain valid for the VM's
    // lifetime (deque storage) — effects bind their host variables once.
    // reg00..reg99 resolve to process-global shared registers (the AVS
    // cross-effect communication channel), not per-VM slots.
    double* var(const std::string& name);

    // megabuf(index) storage: per-VM sparse pages. gmegabuf is process-global
    // (shared by every VM, like the reg's). Index clamped to [0, 8M);
    // out-of-range yields a scratch slot that reads 0. Returned pointers stay
    // valid for the buffer's lifetime.
    double* megabufSlot(double index);
    static double* gmegabufSlot(double index);

    // Direct access to the shared reg00-99 pool (the same slots var("regNN")
    // resolves to) — used by the host to let regs drive effect parameters.
    static double* globalRegSlot(int i);

    // Zero reg00-99 and clear gmegabuf. Called on preset load so presets are
    // deterministic instead of inheriting the previous preset's leftovers.
    static void resetShared();

    // Optional host hooks (AVS audio access). Unset hooks return 0.
    std::function<double(double band, double width, double channel)> getosc;
    std::function<double(double band, double width, double channel)> getspec;
    std::function<double(double start)> gettime;

    double rand01();               // xorshift, uniform [0,1)
    uint32_t rngState = 0x2545F491u;

private:
    std::deque<double> _slots;
    std::unordered_map<std::string, double*> _vars;   // keys lowercased
    std::unordered_map<int, std::vector<double>> _megapages;
};

// AST ops/functions — public so tooling (the EEL→MSL transpiler) can walk
// compiled programs.
enum Op {
    OP_NUM, OP_VAR, OP_ASSIGN,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW, OP_NEG,
    OP_LT, OP_GT, OP_LE, OP_GE, OP_EQ, OP_NE,
    OP_BITAND, OP_BITOR,
    OP_TERNARY,
    OP_CALL,
    OP_BUFASSIGN,                  // megabuf(a)=b / gmegabuf(a)=b; fn = which
};

enum Fn {
    FN_SIN, FN_COS, FN_TAN, FN_ASIN, FN_ACOS, FN_ATAN, FN_ATAN2,
    FN_SQR, FN_SQRT, FN_POW, FN_EXP, FN_LOG, FN_LOG10,
    FN_ABS, FN_MIN, FN_MAX, FN_SIGN, FN_RAND, FN_FLOOR, FN_CEIL,
    FN_INVSQRT, FN_SIGMOID,
    FN_BAND, FN_BOR, FN_BNOT, FN_IF, FN_EQUAL, FN_ABOVE, FN_BELOW,
    FN_EXEC2, FN_EXEC3,
    FN_GETOSC, FN_GETSPEC, FN_GETTIME,
    FN_MEGABUF, FN_GMEGABUF, FN_ASSIGN,
    // extensions beyond ns-eel1 (this project)
    FN_LERP, FN_CLAMP, FN_FRAC, FN_SMOOTHSTEP,
    FN_NOISE, FN_HSV, FN_GETR, FN_GETG, FN_GETB,
};

// Internal AST node (public so the parser translation unit can build them).
struct Node {
    int op = 0;                    // Op enum above
    int a = -1, b = -1, c = -1;
    double num = 0;
    double* slot = nullptr;
    int fn = -1;
};

// A compiled script bound to a VM. Copyable; nodes reference VM slots.
class Program {
public:
    bool ok() const { return _error.empty(); }
    const std::string& error() const { return _error; }
    bool empty() const { return _stmts.empty(); }

    void run() const;              // execute all statements in order

    // AST access for tooling (EEL→MSL transpiler).
    const std::vector<Node>& nodes() const { return _nodes; }
    const std::vector<int>& stmts() const { return _stmts; }

private:
    friend Program compile(VM&, const std::string&);

    VM* _vm = nullptr;
    std::vector<Node> _nodes;
    std::vector<int> _stmts;       // root node per statement
    std::string _error;

    double eval(int idx) const;
};

// Compile `src` against `vm`. On error, Program::ok() is false and error()
// holds a message; run() on a failed program is a no-op.
Program compile(VM& vm, const std::string& src);

}} // namespace viz::eel
