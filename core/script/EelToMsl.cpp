//
// EelToMsl.cpp — see EelToMsl.h.
//
#include "EelToMsl.h"
#include <cstdio>
#include <map>

namespace viz { namespace eel {

// EEL-semantics helpers, prepended to every kernel. Arguments are pass-by-
// value so eager evaluation (if/band/bor/exec) falls out of C call semantics.
static const char* kPrelude = R"MSL(
#include <metal_stdlib>
using namespace metal;
static inline float e_div(float a, float b) { return b == 0.0f ? 0.0f : a / b; }
static inline float e_mod(float a, float b) {
    int ib = (int)b; if (ib == 0) return 0.0f; return (float)((int)a % ib);
}
static inline float e_if(float c, float t, float f) { return c != 0.0f ? t : f; }
static inline float e_band(float a, float b) { return (a != 0.0f && b != 0.0f) ? 1.0f : 0.0f; }
static inline float e_bor(float a, float b) { return (a != 0.0f || b != 0.0f) ? 1.0f : 0.0f; }
static inline float e_bnot(float a) { return a == 0.0f ? 1.0f : 0.0f; }
static inline float e_exec2(float, float b) { return b; }
static inline float e_exec3(float, float, float c) { return c; }
static inline float e_sqr(float a) { return a * a; }
static inline float e_sqrt(float a) { return a <= 0.0f ? 0.0f : sqrt(a); }
static inline float e_asin(float a) { return (a < -1.0f || a > 1.0f) ? 0.0f : asin(a); }
static inline float e_acos(float a) { return (a < -1.0f || a > 1.0f) ? 0.0f : acos(a); }
static inline float e_log(float a) { return a <= 0.0f ? 0.0f : log(a); }
static inline float e_log10(float a) { return a <= 0.0f ? 0.0f : log10(a); }
static inline float e_invsqrt(float a) { return a <= 0.0f ? 0.0f : rsqrt(a); }
static inline float e_sigmoid(float v, float c) {
    float t = 1.0f + exp(-v * c); return t != 0.0f ? 1.0f / t : 0.0f;
}
static inline float e_smoothstep(float e0, float e1, float x) {
    if (e0 == e1) return x < e0 ? 0.0f : 1.0f;
    float t = clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
static inline float e_hash(float ix, float iy) {
    float s = sin(ix * 127.1f + iy * 311.7f) * 43758.5453f;
    return s - floor(s);
}
static inline float e_noise(float x, float y) {
    float ix = floor(x), iy = floor(y);
    float fx = x - ix, fy = y - iy;
    float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy);
    float a = e_hash(ix, iy), b = e_hash(ix + 1.0f, iy);
    float c = e_hash(ix, iy + 1.0f), d = e_hash(ix + 1.0f, iy + 1.0f);
    return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}
static inline float e_hsv(float hh, float s, float v) {
    float h6 = (hh - floor(hh)) * 6.0f;
    float r = clamp(fabs(h6 - 3.0f) - 1.0f, 0.0f, 1.0f);
    float g = clamp(2.0f - fabs(h6 - 2.0f), 0.0f, 1.0f);
    float b = clamp(2.0f - fabs(h6 - 4.0f), 0.0f, 1.0f);
    float qr = floor(clamp(v * (1.0f + s * (r - 1.0f)), 0.0f, 1.0f) * 255.0f + 0.5f);
    float qg = floor(clamp(v * (1.0f + s * (g - 1.0f)), 0.0f, 1.0f) * 255.0f + 0.5f);
    float qb = floor(clamp(v * (1.0f + s * (b - 1.0f)), 0.0f, 1.0f) * 255.0f + 0.5f);
    return qr * 65536.0f + qg * 256.0f + qb;
}
static inline float e_getr(float c) { return (float)(((int)c >> 16) & 255) / 255.0f; }
static inline float e_getg(float c) { return (float)(((int)c >> 8) & 255) / 255.0f; }
static inline float e_getb(float c) { return (float)((int)c & 255) / 255.0f; }
static inline float e_rand(float m, uint2 g, float seed) {
    float v = e_hash((float)g.x + seed * 13.7f, (float)g.y + seed * 7.3f);
    return m < 1.0f ? v : floor(v * m);
}
)MSL";

namespace {
struct Emitter {
    const std::vector<Node>& nodes;
    std::map<double*, std::string> names;   // slot -> emitted name
    std::vector<double*> uniforms;          // discovery order
    std::string err;

    explicit Emitter(const Program& p) : nodes(p.nodes()) {}

    std::string varName(double* slot) {
        auto it = names.find(slot);
        if (it != names.end()) return it->second;
        char buf[16];
        snprintf(buf, sizeof buf, "v%zu", uniforms.size());
        uniforms.push_back(slot);
        names[slot] = buf;
        return buf;
    }

    std::string num(double v) {
        char buf[40];
        snprintf(buf, sizeof buf, "%.9g", v);
        std::string s = buf;
        if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
            s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
            s += ".0";
        return s + "f";
    }

    void fail(const std::string& m) { if (err.empty()) err = m; }

    std::string emit(int idx) {
        if (!err.empty()) return "0.0f";
        const Node& n = nodes[idx];
        auto A = [&] { return emit(n.a); };
        auto B = [&] { return emit(n.b); };
        auto C = [&] { return emit(n.c); };
        switch (n.op) {
            case OP_NUM:    return num(n.num);
            case OP_VAR:    return varName(n.slot);
            case OP_ASSIGN: return "(" + varName(n.slot) + " = " + B() + ")";
            case OP_ADD:    return "(" + A() + " + " + B() + ")";
            case OP_SUB:    return "(" + A() + " - " + B() + ")";
            case OP_MUL:    return "(" + A() + " * " + B() + ")";
            case OP_DIV:    return "e_div(" + A() + ", " + B() + ")";
            case OP_MOD:    return "e_mod(" + A() + ", " + B() + ")";
            case OP_POW:    return "pow(" + A() + ", " + B() + ")";
            case OP_NEG:    return "(-" + A() + ")";
            case OP_LT:     return "((" + A() + " < " + B() + ") ? 1.0f : 0.0f)";
            case OP_GT:     return "((" + A() + " > " + B() + ") ? 1.0f : 0.0f)";
            case OP_LE:     return "((" + A() + " <= " + B() + ") ? 1.0f : 0.0f)";
            case OP_GE:     return "((" + A() + " >= " + B() + ") ? 1.0f : 0.0f)";
            case OP_EQ:     return "((" + A() + " == " + B() + ") ? 1.0f : 0.0f)";
            case OP_NE:     return "((" + A() + " != " + B() + ") ? 1.0f : 0.0f)";
            case OP_BITAND: return "(float)((int)" + A() + " & (int)" + B() + ")";
            case OP_BITOR:  return "(float)((int)" + A() + " | (int)" + B() + ")";
            case OP_TERNARY:return "((" + A() + " != 0.0f) ? (" + B() + ") : (" + C() + "))";
            case OP_BUFASSIGN: fail("megabuf assignment not supported in pixel scripts"); return "0.0f";
            case OP_CALL:   break;
            default:        fail("unsupported op"); return "0.0f";
        }
        switch (n.fn) {
            case FN_SIN:    return "sin(" + A() + ")";
            case FN_COS:    return "cos(" + A() + ")";
            case FN_TAN:    return "tan(" + A() + ")";
            case FN_ASIN:   return "e_asin(" + A() + ")";
            case FN_ACOS:   return "e_acos(" + A() + ")";
            case FN_ATAN:   return "atan(" + A() + ")";
            case FN_ATAN2:  return "atan2(" + A() + ", " + B() + ")";
            case FN_SQR:    return "e_sqr(" + A() + ")";
            case FN_SQRT:   return "e_sqrt(" + A() + ")";
            case FN_POW:    return "pow(" + A() + ", " + B() + ")";
            case FN_EXP:    return "exp(" + A() + ")";
            case FN_LOG:    return "e_log(" + A() + ")";
            case FN_LOG10:  return "e_log10(" + A() + ")";
            case FN_ABS:    return "fabs(" + A() + ")";
            case FN_MIN:    return "fmin(" + A() + ", " + B() + ")";
            case FN_MAX:    return "fmax(" + A() + ", " + B() + ")";
            case FN_SIGN:   return "sign(" + A() + ")";
            case FN_RAND:   return "e_rand(" + A() + ", g, u[0])";
            case FN_FLOOR:  return "floor(" + A() + ")";
            case FN_CEIL:   return "ceil(" + A() + ")";
            case FN_INVSQRT:return "e_invsqrt(" + A() + ")";
            case FN_SIGMOID:return "e_sigmoid(" + A() + ", " + B() + ")";
            case FN_BAND:   return "e_band(" + A() + ", " + B() + ")";
            case FN_BOR:    return "e_bor(" + A() + ", " + B() + ")";
            case FN_BNOT:   return "e_bnot(" + A() + ")";
            case FN_IF:     return "e_if(" + A() + ", " + B() + ", " + C() + ")";
            case FN_EQUAL:  return "((" + A() + " == " + B() + ") ? 1.0f : 0.0f)";
            case FN_ABOVE:  return "((" + A() + " > " + B() + ") ? 1.0f : 0.0f)";
            case FN_BELOW:  return "((" + A() + " < " + B() + ") ? 1.0f : 0.0f)";
            case FN_EXEC2:  return "e_exec2(" + A() + ", " + B() + ")";
            case FN_EXEC3:  return "e_exec3(" + A() + ", " + B() + ", " + C() + ")";
            case FN_LERP:   return "mix(" + A() + ", " + B() + ", " + C() + ")";
            case FN_CLAMP:  return "clamp(" + A() + ", " + B() + ", " + C() + ")";
            case FN_FRAC:   return "fract(" + A() + ")";
            case FN_SMOOTHSTEP: return "e_smoothstep(" + A() + ", " + B() + ", " + C() + ")";
            case FN_NOISE:  return "e_noise(" + A() + ", " + B() + ")";
            case FN_HSV:    return "e_hsv(" + A() + ", " + B() + ", " + C() + ")";
            case FN_GETR:   return "e_getr(" + A() + ")";
            case FN_GETG:   return "e_getg(" + A() + ")";
            case FN_GETB:   return "e_getb(" + A() + ")";
            case FN_GETOSC: fail("getosc not supported in pixel scripts"); return "0.0f";
            case FN_GETSPEC:fail("getspec not supported in pixel scripts (use bass/mid/treb)"); return "0.0f";
            case FN_GETTIME:fail("gettime not supported in pixel scripts (use t)"); return "0.0f";
            case FN_MEGABUF: case FN_GMEGABUF:
                fail("megabuf not supported in pixel scripts"); return "0.0f";
            case FN_ASSIGN: fail("assign() not supported in pixel scripts"); return "0.0f";
            default:        fail("unsupported function"); return "0.0f";
        }
    }
};
} // anonymous namespace

MslResult transpileToMsl(const Program& prog,
                         const std::vector<std::pair<double*, const char*>>& pixelVars)
{
    MslResult out;
    if (!prog.ok()) { out.error = prog.error(); return out; }

    Emitter em(prog);
    for (const auto& [slot, name] : pixelVars) em.names[slot] = name;

    std::string body;
    for (int root : prog.stmts()) {
        std::string e = em.emit(root);
        if (!em.err.empty()) { out.error = em.err; return out; }
        body += "    (void)(" + e + ");\n";
    }

    // locals for every script variable, initialized from the frame snapshot
    std::string locals;
    for (size_t i = 0; i < em.uniforms.size(); ++i) {
        char buf[64];
        snprintf(buf, sizeof buf, "    float v%zu = u[%d];\n", i, kMslUniformBase + (int)i);
        locals += buf;
    }

    out.source = std::string(kPrelude) +
R"MSL(
kernel void px_main(texture2d<float, access::read> src [[texture(0)]],
                    texture2d<float, access::write> dst [[texture(1)]],
                    constant float* u [[buffer(0)]],
                    uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    float w = (float)dst.get_width(), h = (float)dst.get_height();
    float x = ((float)g.x + 0.5f - w * 0.5f) / u[1];   // u[1],u[2]: axis scales
    float y = ((float)g.y + 0.5f - h * 0.5f) / u[2];   // (aspect modes)
    float d = sqrt(x * x + y * y) * 0.70710678f;
    float r = atan2(y, x) + 1.57079633f;
    float4 _c = src.read(g);
    float red = _c.r, green = _c.g, blue = _c.b;
    (void)d; (void)r; (void)x; (void)y; (void)w; (void)h;
)MSL" + locals + body +
R"MSL(    dst.write(float4(clamp(red, 0.0f, 1.0f), clamp(green, 0.0f, 1.0f),
                     clamp(blue, 0.0f, 1.0f), 1.0f), g);
}
)MSL";
    out.uniforms = std::move(em.uniforms);
    return out;
}

}} // namespace viz::eel
