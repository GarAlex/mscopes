//
// Picture.cpp — see Picture.h.
//
#include "Picture.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace viz {

namespace {

std::atomic<uint64_t> gLookVersion{0};
uint64_t nextVersion() { return ++gLookVersion; }

inline float clamp01(float v) { return std::min(1.f, std::max(0.f, v)); }
inline float mixf(float a, float b, float t) { return a + (b - a) * t; }

// sRGB transfer, for looks that are defined on light (white balance).
inline float toLinear(float v)
{
    v = clamp01(v);
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}
inline float toDisplay(float v)
{
    v = std::max(0.f, v);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}
inline float luma(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

// White balance in linear light with the luminance kept, then back.
void whiteBalance(const float in[3], float out[3], float gr, float gg, float gb)
{
    float l[3] = { toLinear(in[0]), toLinear(in[1]), toLinear(in[2]) };
    float y0 = luma(l);
    float w[3] = { l[0] * gr, l[1] * gg, l[2] * gb };
    float y1 = luma(w);
    float k = y1 > 1e-6f ? y0 / y1 : 1.f;
    for (int i = 0; i < 3; ++i) out[i] = clamp01(toDisplay(w[i] * k));
}

void saturate(float c[3], float amount)
{
    float y = luma(c);
    for (int i = 0; i < 3; ++i) c[i] = y + (c[i] - y) * amount;
}

// A soft S-curve around mid grey; amount 0 = none.
inline float sCurve(float v, float amount)
{
    float s = v * v * (3.f - 2.f * v);
    return mixf(v, s, amount);
}

// ---- the looks: display values in, display values out -------------------
using LookFn = void (*)(const float in[3], float out[3]);

void lookOff(const float in[3], float out[3]) { for (int i = 0; i < 3; ++i) out[i] = in[i]; }

// Warm and Cold as colour grades. A photographic white-balance shift alone
// can't make neon cyan read as warm — its green stays dominant — so the
// white-balanced colour is mixed with a ramp of warm (or cold) colours
// picked by the pixel's luminance. The ramp colour is rescaled to that same
// luminance, so brightness is kept and black stays black (the screen off);
// the mix keeps some of the preset's own hue variety.
struct RampStop { float y; float rgb[3]; };

void rampColor(const RampStop* stops, int n, float y, float out[3])
{
    int i = 0;
    while (i < n - 2 && y > stops[i + 1].y) ++i;
    float t = clamp01((y - stops[i].y) / std::max(1e-4f, stops[i + 1].y - stops[i].y));
    for (int c = 0; c < 3; ++c) out[c] = mixf(stops[i].rgb[c], stops[i + 1].rgb[c], t);
    float ly = luma(out);
    float k = ly > 1e-5f ? y / ly : 0.f;
    for (int c = 0; c < 3; ++c) out[c] *= k;
}

void gradedBalance(const float in[3], float out[3], float gr, float gg, float gb,
                   const RampStop* ramp, int n, float amount)
{
    whiteBalance(in, out, gr, gg, gb);
    float y = clamp01(luma(out));
    float r[3];
    rampColor(ramp, n, y, r);
    for (int i = 0; i < 3; ++i) out[i] = clamp01(mixf(out[i], r[i], amount));
}

void lookWarm(const float in[3], float out[3])
{
    static const RampStop ramp[] = {
        { 0.00f, { 0.60f, 0.20f, 0.05f } },   // deep ember (scaled to black at y=0)
        { 0.30f, { 1.00f, 0.42f, 0.12f } },   // orange
        { 0.65f, { 1.00f, 0.72f, 0.38f } },   // amber gold
        { 1.00f, { 1.00f, 0.94f, 0.80f } },   // cream
    };
    gradedBalance(in, out, 1.15f, 0.97f, 0.74f, ramp, 4, 0.55f);
}

void lookCold(const float in[3], float out[3])
{
    static const RampStop ramp[] = {
        { 0.00f, { 0.05f, 0.12f, 0.60f } },   // deep blue (scaled to black at y=0)
        { 0.30f, { 0.12f, 0.40f, 1.00f } },   // blue
        { 0.65f, { 0.40f, 0.78f, 1.00f } },   // ice
        { 1.00f, { 0.86f, 0.95f, 1.00f } },   // frost white
    };
    gradedBalance(in, out, 0.84f, 0.98f, 1.16f, ramp, 4, 0.55f);
}

void lookSepia(const float in[3], float out[3])
{
    // Luminance through a toned ramp: warm near-black, brown mids, cream highs.
    const float dark[3]  = { 0.018f, 0.010f, 0.004f };
    const float mid[3]   = { 0.620f, 0.470f, 0.320f };
    const float light[3] = { 1.000f, 0.960f, 0.880f };
    float y = clamp01(luma(in));
    for (int i = 0; i < 3; ++i)
        out[i] = y < 0.5f ? mixf(dark[i], mid[i], y * 2.f) : mixf(mid[i], light[i], (y - 0.5f) * 2.f);
}

void lookTealOrange(const float in[3], float out[3])
{
    float c[3] = { in[0], in[1], in[2] };
    float y = clamp01(luma(c));
    // Shadows toward teal, highlights toward orange. The shadow tint fades
    // out toward black so black stays black instead of turning navy.
    float sh = (1.f - y) * (1.f - y) * std::min(1.f, y * 6.f), hi = y * y;
    const float teal[3]   = { -0.05f, 0.015f, 0.07f };
    const float orange[3] = { 0.07f, 0.02f, -0.07f };
    for (int i = 0; i < 3; ++i) c[i] += teal[i] * sh + orange[i] * hi;
    saturate(c, 1.12f);
    for (int i = 0; i < 3; ++i) out[i] = clamp01(sCurve(clamp01(c[i]), 0.25f));
}

void lookNoir(const float in[3], float out[3])
{
    float y = sCurve(clamp01(luma(in)), 0.75f);
    out[0] = out[1] = out[2] = y;
}

void lookBleach(const float in[3], float out[3])
{
    // Bleach bypass: the silver left in the print — the image overlaid with
    // its own luminance, desaturated, contrasty.
    float y = clamp01(luma(in));
    float c[3];
    for (int i = 0; i < 3; ++i) {
        float a = y, b = clamp01(in[i]);
        float ov = a < 0.5f ? 2.f * a * b : 1.f - 2.f * (1.f - a) * (1.f - b);
        c[i] = mixf(b, ov, 0.85f);
    }
    saturate(c, 0.45f);
    for (int i = 0; i < 3; ++i) out[i] = clamp01(c[i]);
}

void lookFaded(const float in[3], float out[3])
{
    // Faded print: lifted blacks, softened whites, gentle split tone.
    float c[3] = { clamp01(in[0]), clamp01(in[1]), clamp01(in[2]) };
    saturate(c, 0.78f);
    float y = clamp01(luma(c));
    for (int i = 0; i < 3; ++i) c[i] = 0.05f + clamp01(c[i]) * 0.88f;   // the fade: blacks lifted on purpose
    c[0] += 0.025f * y;              // warm highlights
    c[2] += 0.030f * (1.f - y);      // cool shadows
    for (int i = 0; i < 3; ++i) out[i] = clamp01(c[i]);
}

void lookVivid(const float in[3], float out[3])
{
    float c[3] = { clamp01(in[0]), clamp01(in[1]), clamp01(in[2]) };
    saturate(c, 1.35f);
    for (int i = 0; i < 3; ++i) out[i] = clamp01(sCurve(clamp01(c[i]), 0.2f));
}

struct LookDef { const char* name; LookFn fn; };
const LookDef kLooks[] = {
    { "Off",           lookOff },
    { "Warm",          lookWarm },
    { "Cold",          lookCold },
    { "Sepia",         lookSepia },
    { "Teal & Orange", lookTealOrange },
    { "Noir",          lookNoir },
    { "Bleach bypass", lookBleach },
    { "Faded film",    lookFaded },
    { "Vivid",         lookVivid },
};
constexpr int kLookCount = (int)(sizeof(kLooks) / sizeof(kLooks[0]));

// Bake any colour function into a kLookSize^3 table.
template <class Fn>
void bake(LookTable& out, Fn&& fn)
{
    const int n = kLookSize;
    std::vector<float> rgb((size_t)n * n * n * 3);
    size_t k = 0;
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                float in[3] = { r / float(n - 1), g / float(n - 1), b / float(n - 1) }, o[3];
                fn(in, o);
                rgb[k++] = clamp01(o[0]); rgb[k++] = clamp01(o[1]); rgb[k++] = clamp01(o[2]);
            }
    out.size = n;
    out.rgb.swap(rgb);
    out.version = nextVersion();
}

// Trilinear lookup into raw table data at table coordinates u in 0..1.
void sampleRaw(const std::vector<float>& rgb, int n, const float u[3], float out[3])
{
    float p[3]; int i0[3], i1[3]; float f[3];
    for (int a = 0; a < 3; ++a) {
        p[a] = clamp01(u[a]) * (n - 1);
        i0[a] = std::min(n - 2, (int)p[a]);
        i1[a] = i0[a] + 1;
        f[a] = p[a] - i0[a];
    }
    auto at = [&](int r, int g, int b, int ch) { return rgb[(((size_t)b * n + g) * n + r) * 3 + ch]; };
    for (int ch = 0; ch < 3; ++ch) {
        float c00 = mixf(at(i0[0], i0[1], i0[2], ch), at(i1[0], i0[1], i0[2], ch), f[0]);
        float c10 = mixf(at(i0[0], i1[1], i0[2], ch), at(i1[0], i1[1], i0[2], ch), f[0]);
        float c01 = mixf(at(i0[0], i0[1], i1[2], ch), at(i1[0], i0[1], i1[2], ch), f[0]);
        float c11 = mixf(at(i0[0], i1[1], i1[2], ch), at(i1[0], i1[1], i1[2], ch), f[0]);
        out[ch] = mixf(mixf(c00, c10, f[1]), mixf(c01, c11, f[1]), f[2]);
    }
}

bool parseFloats(std::istringstream& ss, float* v, int n)
{
    for (int i = 0; i < n; ++i) if (!(ss >> v[i])) return false;
    return true;
}

} // namespace

const std::vector<std::string>& lookNames()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const auto& d : kLooks) v.push_back(d.name);
        return v;
    }();
    return names;
}

bool buildLook(int index, LookTable& out)
{
    if (index < 0 || index >= kLookCount) return false;
    if (index == 0) { out.clear(); out.version = nextVersion(); return true; }
    bake(out, kLooks[index].fn);
    return true;
}

void sampleLook(const LookTable& t, const float in[3], float out[3])
{
    if (t.empty()) { for (int i = 0; i < 3; ++i) out[i] = in[i]; return; }
    sampleRaw(t.rgb, t.size, in, out);
}

bool parseCube(const std::string& text, LookTable& out, std::string* err)
{
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    int size3 = 0, size1 = 0;
    float dmin[3] = { 0, 0, 0 }, dmax[3] = { 1, 1, 1 };
    std::vector<float> data;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue;
        std::istringstream ss(line.substr(start));
        std::string key;
        ss >> key;
        if (key == "TITLE") continue;
        if (key == "LUT_3D_SIZE") {
            if (!(ss >> size3) || size3 < 2 || size3 > 256) return fail("LUT_3D_SIZE must be 2 to 256");
        } else if (key == "LUT_1D_SIZE") {
            if (!(ss >> size1) || size1 < 2 || size1 > 65536) return fail("LUT_1D_SIZE must be 2 to 65536");
        } else if (key == "DOMAIN_MIN") {
            if (!parseFloats(ss, dmin, 3)) return fail("bad DOMAIN_MIN on line " + std::to_string(lineNo));
        } else if (key == "DOMAIN_MAX") {
            if (!parseFloats(ss, dmax, 3)) return fail("bad DOMAIN_MAX on line " + std::to_string(lineNo));
        } else if (key == "LUT_3D_INPUT_RANGE" || key == "LUT_1D_INPUT_RANGE") {
            float r[2];
            if (!parseFloats(ss, r, 2)) return fail("bad " + key + " on line " + std::to_string(lineNo));
            for (int i = 0; i < 3; ++i) { dmin[i] = r[0]; dmax[i] = r[1]; }
        } else {
            // A data row: three numbers. Anything else is an unknown keyword
            // (ignored, as the format allows) — unless it looks numeric.
            char* end = nullptr;
            float v0 = std::strtof(key.c_str(), &end);
            if (!end || *end != '\0') continue;
            float v[2];
            if (!parseFloats(ss, v, 2)) return fail("line " + std::to_string(lineNo) + " needs three numbers");
            data.push_back(v0); data.push_back(v[0]); data.push_back(v[1]);
        }
    }
    if (size3 && size1) return fail("both a 1D and a 3D size are declared");
    if (!size3 && !size1) return fail("no LUT_3D_SIZE or LUT_1D_SIZE");
    for (int i = 0; i < 3; ++i)
        if (!(dmax[i] > dmin[i])) return fail("DOMAIN_MAX must be above DOMAIN_MIN");
    const size_t want = size3 ? (size_t)size3 * size3 * size3 * 3 : (size_t)size1 * 3;
    if (data.size() != want)
        return fail("expected " + std::to_string(want / 3) + " rows, found " + std::to_string(data.size() / 3));

    LookTable baked;
    auto toTable = [&](const float x[3], float u[3]) {
        for (int i = 0; i < 3; ++i) u[i] = clamp01((x[i] - dmin[i]) / (dmax[i] - dmin[i]));
    };
    if (size3) {
        bake(baked, [&](const float in[3], float o[3]) {
            float u[3]; toTable(in, u);
            sampleRaw(data, size3, u, o);
        });
    } else {
        bake(baked, [&](const float in[3], float o[3]) {
            float u[3]; toTable(in, u);
            for (int ch = 0; ch < 3; ++ch) {
                float p = u[ch] * (size1 - 1);
                int i0 = std::min(size1 - 2, (int)p);
                float f = p - i0;
                o[ch] = mixf(data[(size_t)i0 * 3 + ch], data[(size_t)(i0 + 1) * 3 + ch], f);
            }
        });
    }
    out = std::move(baked);
    return true;
}

bool loadCubeFile(const std::string& path, LookTable& out, std::string* err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (ss.str().size() > 64 * 1024 * 1024) { if (err) *err = "file too large"; return false; }
    return parseCube(ss.str(), out, err);
}

} // namespace viz
