//
// TransFilter.cpp — convolution / water / filter family, ported from classic
// AVS (vis_avs, BSD-3-Clause; see TransFilter.h for attribution).
//
#include "TransFilter.h"
#include "GpuFx.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace viz {

// ---------------------------------------------------------------------------
// ConvolutionEffect — our take on AVS Convolution Filter (e_convolution.cpp,
// BSD-3). Faithful core: out = (sum(kernel * src) / scale) + bias, clamped,
// with optional wrap addressing and (per preset) absolute value. The original
// JIT-compiled x86 for a free-form 7x7 grid; we use classic 5x5 presets.

namespace {

struct ConvoPreset {
    float k[25];       // row-major 5x5
    float scale;       // divide accumulated sum by this (original's scale)
    float bias;        // preset bias added after scaling (original's bias/256)
    bool  absolute;    // original's "absolute value" option
};

const ConvoPreset kConvoPresets[6] = {
    // identity
    {{0,0,0,0,0, 0,0,0,0,0, 0,0,1,0,0, 0,0,0,0,0, 0,0,0,0,0}, 1.f, 0.f, false},
    // box blur
    {{1,1,1,1,1, 1,1,1,1,1, 1,1,1,1,1, 1,1,1,1,1, 1,1,1,1,1}, 25.f, 0.f, false},
    // gaussian (binomial [1 4 6 4 1] outer product, scale 256)
    {{1,4,6,4,1, 4,16,24,16,4, 6,24,36,24,6, 4,16,24,16,4, 1,4,6,4,1},
     256.f, 0.f, false},
    // sharpen (3x3 cross embedded)
    {{0,0,0,0,0, 0,0,-1,0,0, 0,-1,5,-1,0, 0,0,-1,0,0, 0,0,0,0,0}, 1.f, 0.f, false},
    // edge detect (3x3 laplacian, zero-sum -> scale 1, absolute like original)
    {{0,0,0,0,0, 0,-1,-1,-1,0, 0,-1,8,-1,0, 0,-1,-1,-1,0, 0,0,0,0,0},
     1.f, 0.f, true},
    // emboss (3x3 directional, bias 0.5 to center on gray)
    {{0,0,0,0,0, 0,-2,-1,0,0, 0,-1,1,1,0, 0,0,1,2,0, 0,0,0,0,0}, 1.f, 0.5f, false},
};

inline int wrapOrClamp(int v, int n, bool wrap) {
    if (wrap) { v %= n; if (v < 0) v += n; return v; }
    return std::clamp(v, 0, n - 1);
}

} // namespace

bool ConvolutionEffect::isGpu() const { return gpu::available(); }

void ConvolutionEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                               const VizFrame& /*a*/, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;

    const ConvoPreset& p =
        kConvoPresets[std::clamp((int)kernelMode, 0, 5)];
    const bool doWrap = wrap > 0.5f;
    const float invScale = 1.f / (p.scale != 0.f ? p.scale : 1.f);
    const float mixT = std::clamp(strength, 0.f, 1.f);

    if (gpu::available()) {
        gpu::convolve5(cur, p.k, invScale, p.bias + bias, p.absolute, doWrap, mixT);
        return;
    }

    // CPU path. Scratch copy of the source frame (the original double-buffered too).
    _src.assign(cur.px.begin(), cur.px.end());

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float acc[3] = {0.f, 0.f, 0.f};
            for (int ky = -2; ky <= 2; ++ky) {
                int sy = wrapOrClamp(y + ky, H, doWrap);
                for (int kx = -2; kx <= 2; ++kx) {
                    float wgt = p.k[(ky + 2) * 5 + (kx + 2)];
                    if (wgt == 0.f) continue;
                    int sx = wrapOrClamp(x + kx, W, doWrap);
                    const float* s = &_src[((size_t)sy * W + sx) * 4];
                    acc[0] += wgt * s[0];
                    acc[1] += wgt * s[1];
                    acc[2] += wgt * s[2];
                }
            }
            float* d = cur.at(x, y);
            for (int c = 0; c < 3; ++c) {
                float v = acc[c] * invScale + p.bias + bias;
                if (p.absolute) v = std::fabs(v);
                v = std::clamp(v, 0.f, 1.f);
                d[c] = d[c] + (v - d[c]) * mixT;
            }
            d[3] = 1.f;
        }
    }
    quantize8(cur.px);            // 8-bit truncation, as AVS (Framebuffer.h)
}

// ---------------------------------------------------------------------------
// MultiFilterEffect — our take on AVS Multi Filter (e_multifilter.cpp, BSD-3).
// chrome fold: chan < 128 ? chan*2 : 510 - chan*2 (in 0..1: c<.5 ? 2c : 2-2c),
// applied 1/2/3 times. infroot border convolution: any non-black pixel goes
// white and also whitens the pixel to its left and the one above — faithfully
// keeping the original APE's lopsided in-place forward scan.

void MultiFilterEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                               const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;

    if (toggleOnBeat > 0.5f) {
        if (a.beat) _toggleState = !_toggleState;
        if (!_toggleState) return;
    }

    const int m = std::clamp((int)mode, 0, 3);
    if (m <= 2) {
        const int passes = m + 1;                 // chrome / double / triple
        const size_t n = cur.px.size();
        for (size_t i = 0; i < n; i += 4) {
            for (int c = 0; c < 3; ++c) {
                float v = cur.px[i + c];
                for (int pass = 0; pass < passes; ++pass)
                    v = v < 0.5f ? v * 2.f : 2.f - v * 2.f;
                cur.px[i + c] = std::clamp(v, 0.f, 1.f);
            }
        }
    } else {
        // infroot + "small border convolution" mask
        const float thresh = 0.5f / 255.f;        // any color bit set
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                float* p = cur.at(x, y);
                if (p[0] > thresh || p[1] > thresh || p[2] > thresh) {
                    p[0] = p[1] = p[2] = 1.f;
                    if (y > 0) {
                        float* up = cur.at(x, y - 1);
                        up[0] = up[1] = up[2] = 1.f;
                    }
                    if (x > 0) {
                        float* lf = cur.at(x - 1, y);
                        lf[0] = lf[1] = lf[2] = 1.f;
                    }
                } else {
                    p[0] = p[1] = p[2] = 0.f;
                }
                p[3] = 1.f;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// NormaliseEffect — our take on AVS Normalise (e_normalise.cpp, BSD-3).
// Scan the frame's min/max channel value, then linearly remap so min -> 0 and
// max -> 1. Skipped when the frame already spans the full range; a flat frame
// goes black (the original's zeroed scale table).

void NormaliseEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                             const VizFrame& /*a*/, const EffectContext& /*ctx*/)
{
    if (cur.px.empty()) return;

    float minC = 1.f, maxC = 0.f;
    const size_t n = cur.px.size();
    for (size_t i = 0; i < n; i += 4) {
        for (int c = 0; c < 3; ++c) {
            float v = cur.px[i + c];
            if (v > maxC) maxC = v;
            if (v < minC) minC = v;
        }
        if (maxC >= 1.f && minC <= 0.f) return;   // already full range: bail
    }

    const float mixT = std::clamp(mix, 0.f, 1.f);
    const float range = maxC - minC;
    if (range <= 0.f) {
        // flat frame: original's scale table is all zeroes -> black
        for (size_t i = 0; i < n; i += 4)
            for (int c = 0; c < 3; ++c)
                cur.px[i + c] *= (1.f - mixT);
        return;
    }
    const float scale = 1.f / range;
    for (size_t i = 0; i < n; i += 4) {
        for (int c = 0; c < 3; ++c) {
            float v = (cur.px[i + c] - minC) * scale;
            cur.px[i + c] += (std::clamp(v, 0.f, 1.f) - cur.px[i + c]) * mixT;
        }
    }
}

// ---------------------------------------------------------------------------
// WaterEffect — our take on AVS Water (e_water.cpp, BSD-3). For each pixel:
// (sum of the 4 neighbors in the incoming frame) / 2, minus the pixel's value
// from the frame before this one, clamped. Corners sum 2 neighbors unhalved
// and edges 3 neighbors halved, exactly like the original's border cases. The
// incoming frame then becomes the new "last" frame — classic two-buffer wave
// propagation that ripples over the feedback chain.

void WaterEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                         const VizFrame& /*a*/, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W < 2 || H < 2) return;

    const size_t n = cur.px.size();
    if (_lw != W || _lh != H) {                    // (re)init on resize
        _last.assign(n, 0.f);
        _lw = W; _lh = H;
    }
    _out.resize(n);

    auto pix = [&](int x, int y) { return &cur.px[((size_t)y * W + x) * 4]; };

    for (int y = 0; y < H; ++y) {
        const bool top = (y == 0), bottom = (y == H - 1);
        for (int x = 0; x < W; ++x) {
            const bool left = (x == 0), right = (x == W - 1);
            float sum[3] = {0.f, 0.f, 0.f};
            int   nn = 0;
            if (!left)   { const float* s = pix(x - 1, y); sum[0]+=s[0]; sum[1]+=s[1]; sum[2]+=s[2]; ++nn; }
            if (!right)  { const float* s = pix(x + 1, y); sum[0]+=s[0]; sum[1]+=s[1]; sum[2]+=s[2]; ++nn; }
            if (!top)    { const float* s = pix(x, y - 1); sum[0]+=s[0]; sum[1]+=s[1]; sum[2]+=s[2]; ++nn; }
            if (!bottom) { const float* s = pix(x, y + 1); sum[0]+=s[0]; sum[1]+=s[1]; sum[2]+=s[2]; ++nn; }
            // Original: corners (2 neighbors) are NOT halved; everywhere else is.
            // Damping 0.985 stands in for the energy the original's integer
            // truncation removed each step; pure floats otherwise slowly blow
            // up to white under continuous input.
            const float half = ((nn == 2) ? 1.f : 0.5f) * 0.985f;
            const size_t i = ((size_t)y * W + x) * 4;
            for (int c = 0; c < 3; ++c)
                _out[i + c] = std::clamp(sum[c] * half - _last[i + c], 0.f, 1.f);
            _out[i + 3] = 1.f;
        }
    }

    _last.assign(cur.px.begin(), cur.px.end());    // this input is next "last"
    cur.px.swap(_out);
}

// ---------------------------------------------------------------------------
// WaterBumpEffect — our take on AVS Water Bump (e_waterbump.cpp, BSD-3).
// Two ping-pong height fields; each frame the current field's gradient
// displaces the frame (dx/8, dy/8 pixels — the original's >>3), then the
// 8-neighbor update newh = sum/4 - self, damped by 2^-fluidity, writes the
// next field. Beats drop a cosine blob (the original's SineBlob, including
// its (cos + 0xffff)>>19 amplitude quirk).

float WaterBumpEffect::frand() {
    _rng ^= _rng << 13; _rng ^= _rng >> 17; _rng ^= _rng << 5;
    return (float)(_rng & 0xffffff) / 16777216.f;
}

void WaterBumpEffect::sineBlob(int x, int y, int radius, float height) {
    const int W = _bw, H = _bh;
    if (radius < 1) radius = 1;
    if (x < 0) {
        int span = W - 2 * radius - 1;
        x = span > 0 ? 1 + radius + (int)(frand() * span) : W / 2;
    }
    if (y < 0) {
        int span = H - 2 * radius - 1;
        y = span > 0 ? 1 + radius + (int)(frand() * span) : H / 2;
    }
    const int radsq = radius * radius;
    const float length = (1024.f / radius) * (1024.f / radius);
    int left = -radius, right = radius, top = -radius, bottom = radius;
    if (x - radius < 1)     left   -= (x - radius - 1);
    if (y - radius < 1)     top    -= (y - radius - 1);
    if (x + radius > W - 1) right  -= (x + radius - W + 1);
    if (y + radius > H - 1) bottom -= (y + radius - H + 1);

    std::vector<float>& buf = _height[_page];
    for (int cy = top; cy < bottom; ++cy) {
        for (int cx = left; cx < right; ++cx) {
            int square = cy * cy + cx * cx;
            if (square < radsq) {
                float dist = std::sqrt((float)square * length);
                buf[(size_t)W * (cy + y) + cx + x] +=
                    (std::cos(dist) + 65535.f) * height * (1.f / 524288.f);
            }
        }
    }
}

void WaterBumpEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                             const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W < 3 || H < 3) return;

    if (_bw != W || _bh != H) {                    // (re)init on resize
        _height[0].assign((size_t)W * H, 0.f);
        _height[1].assign((size_t)W * H, 0.f);
        _bw = W; _bh = H;
        _page = 0;
    }

    if (a.beat) {
        const int maxDim = std::max(W, H);
        const int radius = std::max(1, (int)(dropRadius * maxDim / 100.f));
        if (random > 0.5f) {
            sineBlob(-1, -1, radius, -depth);
        } else {
            static const float fx[3] = {0.25f, 0.5f, 0.75f};
            int x = (int)(W * fx[std::clamp((int)dropPosX, 0, 2)]);
            int y = (int)(H * fx[std::clamp((int)dropPosY, 0, 2)]);
            sineBlob(x, y, radius, -depth);
        }
    }

    // Displace the frame by the height gradient (dx/8, dy/8 like the >>3).
    _scratch.assign(cur.px.begin(), cur.px.end());
    const std::vector<float>& hgt = _height[_page];
    const long len = (long)W * H;
    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            const long off = (long)y * W + x;
            float dx = hgt[off] - hgt[off + 1];
            float dy = hgt[off] - hgt[off + W];
            long ofs = off + (long)W * (long)std::floor(dy * 0.125f)
                           + (long)std::floor(dx * 0.125f);
            if (ofs < 0 || ofs >= len) ofs = off;  // original's bounds check
            const float* s = &_scratch[(size_t)ofs * 4];
            float* d = cur.at(x, y);
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 1.f;
        }
    }

    // CalcWater into the other page: newh = 8-neighbor sum / 4 - self, damped.
    const float damp = 1.f - std::exp2(-(float)std::max(1, (int)fluidity));
    std::vector<float>& np = _height[1 - _page];
    const std::vector<float>& op = _height[_page];
    for (int y = 1; y < H - 1; ++y) {
        const size_t row = (size_t)y * W;
        for (int x = 1; x < W - 1; ++x) {
            const size_t i = row + x;
            float newh = (op[i + W] + op[i - W] + op[i + 1] + op[i - 1]
                        + op[i - W - 1] + op[i - W + 1]
                        + op[i + W - 1] + op[i + W + 1]) * 0.25f
                       - np[i];
            np[i] = newh * damp;
        }
    }
    _page = 1 - _page;
}

// ---------------------------------------------------------------------------
// BumpEffect — our take on AVS Bump (e_bump.cpp, BSD-3). Height = max channel
// (optionally inverted); per pixel the finite-difference slope is compared to
// the direction of a light: d = 127 - |grad*255 - lightDelta|; when both axes
// are positive, brightness += xd*yd*depth/6400 (the original's fixed-point
// (xd*yd*(depth<<8/100))>>14). The EEL-scripted light path becomes an orbit
// around the center; the on-beat depth jump decays linearly over beat_dur
// frames like the original's fadeout.

void BumpEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                        const VizFrame& a, const EffectContext& ctx)
{
    const int W = cur.w, H = cur.h;
    if (W < 3 || H < 3) return;

    const bool beatOn = onBeat > 0.5f;
    if (beatOn && a.beat) {
        _curDepth   = beatDepth;
        _beatFrames = std::max(1, (int)beatDur);
    } else if (_beatFrames == 0) {
        _curDepth = depth;
    }

    // Light orbits the center.
    const float ang = (float)(ctx.time * orbitSpeed * 2.0 * 3.14159265358979);
    const float rad = orbitRadius * (float)std::min(W, H);
    const int cx = std::clamp((int)(W * 0.5f + std::cos(ang) * rad), 0, W - 1);
    const int cy = std::clamp((int)(H * 0.5f + std::sin(ang) * rad), 0, H - 1);

    const bool inv = invert > 0.5f;
    const int  blend = std::clamp((int)blendMode, 0, 2);
    const float depthScale = _curDepth / 6400.f;   // (xd*yd*depth)/6400 in 0..255
    const float farClamp = 254.f / 255.f;          // original's set_far_depth

    auto heightAt = [&](int x, int y) -> float {   // max channel, 0..255 units
        const float* p = cur.at(x, y);
        float m = std::max(p[0], std::max(p[1], p[2]));
        return (inv ? 1.f - m : m) * 255.f;
    };

    _out.assign(cur.px.begin(), cur.px.end());     // borders pass through

    for (int y = 1; y < H - 1; ++y) {
        const float lightY = (float)(y - cy);
        for (int x = 1; x < W - 1; ++x) {
            float xd = heightAt(x + 1, y) - heightAt(x - 1, y) - (float)(x - cx);
            float yd = heightAt(x, y + 1) - heightAt(x, y - 1) - lightY;
            xd = 127.f - std::fabs(xd);
            yd = 127.f - std::fabs(yd);
            const float* s = cur.at(x, y);
            float lit[3];
            if (xd <= 0.f || yd <= 0.f) {
                for (int c = 0; c < 3; ++c)        // far from light: just clamp
                    lit[c] = std::min(s[c], farClamp);
            } else {
                const float add = xd * yd * depthScale * (1.f / 255.f);
                for (int c = 0; c < 3; ++c)
                    lit[c] = std::min(s[c] + add, farClamp);
            }
            float* d = &_out[((size_t)y * W + x) * 4];
            switch (blend) {
                case 1:  for (int c = 0; c < 3; ++c) d[c] = std::min(1.f, lit[c] + s[c]); break;
                case 2:  for (int c = 0; c < 3; ++c) d[c] = (lit[c] + s[c]) * 0.5f; break;
                default: for (int c = 0; c < 3; ++c) d[c] = lit[c]; break;
            }
            d[3] = 1.f;
        }
    }
    cur.px.swap(_out);

    // On-beat depth decays linearly back toward the base depth.
    if (_beatFrames > 0) {
        --_beatFrames;
        if (_beatFrames) {
            float step = std::fabs(depth - beatDepth) / std::max(1.f, beatDur);
            _curDepth += step * (beatDepth > depth ? -1.f : 1.f);
        }
    }
}

} // namespace viz
