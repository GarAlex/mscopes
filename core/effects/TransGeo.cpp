//
// TransGeo.cpp — geometric transform effects ported from classic AVS.
// Algorithms adapted from the open-sourced AVS codebase (BSD-3, Nullsoft);
// see each effect for the e_<name>.cpp it derives from.
//
#include "TransGeo.h"
#include "GpuFx.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace viz {

static constexpr float kPi = 3.14159265358979f;

// Cheap xorshift32 → 0..1 float (same generator StarfieldEffect uses).
static inline float xrand(uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (s & 0xFFFFFF) / (float)0x1000000;
}
static inline uint32_t xrandu(uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

// Bilinear sample with wrapping (tiling) source coordinates — Framebuffer's
// own sample() clamps, but Roto Blitter / Movement's wrap mode need tiling.
static void sampleWrap(const Framebuffer& fb, float fx, float fy, float out[4]) {
    const int w = fb.w, h = fb.h;
    fx = std::fmod(fx, (float)w); if (fx < 0) fx += w;
    fy = std::fmod(fy, (float)h); if (fy < 0) fy += h;
    int x0 = (int)fx, y0 = (int)fy;
    if (x0 >= w) x0 = w - 1;               // fmod edge safety
    if (y0 >= h) y0 = h - 1;
    int x1 = (x0 + 1) % w, y1 = (y0 + 1) % h;
    float tx = fx - x0, ty = fy - y0;
    const float* p00 = fb.at(x0, y0); const float* p10 = fb.at(x1, y0);
    const float* p01 = fb.at(x0, y1); const float* p11 = fb.at(x1, y1);
    for (int c = 0; c < 4; ++c) {
        float a = p00[c] + (p10[c] - p00[c]) * tx;
        float b = p01[c] + (p11[c] - p01[c]) * tx;
        out[c] = a + (b - a) * ty;
    }
}

// ---------------------------------------------------------------------------
// Mirror — from e_mirror.cpp (BSD-3). The original kept per-direction 0..16
// blend divisors stepping toward a target; we keep float weights 0..1 and the
// same in-place half-copy passes in the same order (l→r, r→l, t→b, b→t).
void MirrorEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                          const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W < 2 || H < 2) return;

    if (onBeatRandom > 0.5f) {
        if (a.beat) {
            // Random pick per orientation: none / one direction / the other,
            // evenly distributed like the original's random_mode().
            uint32_t r = xrandu(_rng);
            int v = r % 3, hz = (r >> 8) % 3;
            _tgt[2] = (v == 2) ? 1.f : 0.f;   // t→b
            _tgt[3] = (v == 1) ? 1.f : 0.f;   // b→t
            _tgt[0] = (hz == 2) ? 1.f : 0.f;  // l→r
            _tgt[1] = (hz == 1) ? 1.f : 0.f;  // r→l
        }
    } else {
        int m = (int)mode;
        _tgt[0] = (m == 1 || m == 5) ? 1.f : 0.f;
        _tgt[1] = (m == 2) ? 1.f : 0.f;
        _tgt[2] = (m == 3 || m == 5) ? 1.f : 0.f;
        _tgt[3] = (m == 4) ? 1.f : 0.f;
    }

    // Step current weights toward targets (transition <= 0 snaps, as when the
    // original's smooth-transition option was off).
    for (int i = 0; i < 4; ++i) {
        if (transition <= 0.f) { _cur[i] = _tgt[i]; continue; }
        float d = _tgt[i] - _cur[i];
        d = std::clamp(d, -transition, transition);
        _cur[i] += d;
    }

    const int halfW = W / 2, halfH = H / 2;
    auto mixPx = [](float* dst, const float* src, float wgt) {
        dst[0] += (src[0] - dst[0]) * wgt;
        dst[1] += (src[1] - dst[1]) * wgt;
        dst[2] += (src[2] - dst[2]) * wgt;
    };

    if (_cur[0] > 0.f) {                       // left → right
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < halfW; ++x)
                mixPx(cur.at(W - 1 - x, y), cur.at(x, y), _cur[0]);
    }
    if (_cur[1] > 0.f) {                       // right → left
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < halfW; ++x)
                mixPx(cur.at(x, y), cur.at(W - 1 - x, y), _cur[1]);
    }
    if (_cur[2] > 0.f) {                       // top → bottom
        for (int y = 0; y < halfH; ++y)
            for (int x = 0; x < W; ++x)
                mixPx(cur.at(x, H - 1 - y), cur.at(x, y), _cur[2]);
    }
    if (_cur[3] > 0.f) {                       // bottom → top
        for (int y = 0; y < halfH; ++y)
            for (int x = 0; x < W; ++x)
                mixPx(cur.at(x, y), cur.at(x, H - 1 - y), _cur[3]);
    }
}

// ---------------------------------------------------------------------------
// Mosaic — from e_mosaic.cpp (BSD-3). cur_size = number of blocks across; each
// block is filled from a pixel at its center (the original started its
// fixed-point steppers at step/2). On beat: jump to on_beat_size, then walk
// back |size - on_beat_size| / duration per frame, as the original did.
void MosaicEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                          const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;

    if (onBeat > 0.5f && a.beat) {
        _curSize  = onBeatSize;
        _cooldown = (int)duration;
    } else if (_cooldown == 0) {
        _curSize = size;
    }

    int cells = std::max(2, (int)_curSize);
    if (cells < 100) {
        if ((int)_scratch.size() < W * H * 4) _scratch.resize((size_t)W * H * 4);
        std::memcpy(_scratch.data(), cur.px.data(), (size_t)W * H * 4 * sizeof(float));

        const int bm = (int)blend;
        for (int y = 0; y < H; ++y) {
            int by = y * cells / H;
            int sy = std::min(H - 1, (by * H + H / 2) / cells);   // block center row
            for (int x = 0; x < W; ++x) {
                int bx = x * cells / W;
                int sx = std::min(W - 1, (bx * W + W / 2) / cells);
                const float* s = &_scratch[((size_t)sy * W + sx) * 4];
                float* d = cur.at(x, y);
                if (bm == 1) {              // additive
                    d[0] = std::min(1.f, d[0] + s[0]);
                    d[1] = std::min(1.f, d[1] + s[1]);
                    d[2] = std::min(1.f, d[2] + s[2]);
                } else if (bm == 2) {       // 50/50
                    d[0] = (d[0] + s[0]) * 0.5f;
                    d[1] = (d[1] + s[1]) * 0.5f;
                    d[2] = (d[2] + s[2]) * 0.5f;
                } else {                    // replace
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                }
            }
        }
    }

    if (_cooldown) {
        _cooldown--;
        if (_cooldown > 0) {
            float step = std::fabs(size - onBeatSize) / std::max(1.f, duration);
            _curSize += step * (onBeatSize > size ? -1.f : 1.f);
        }
    }
}

// ---------------------------------------------------------------------------
// Scatter — from e_scatter.cpp (BSD-3). 512-entry fudge table of small ±3px
// offsets, applied to every pixel except the top and bottom 4 rows (kept so
// the offsets never leave the buffer — same trick as the original).
void ScatterEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                           const VizFrame& /*a*/, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H <= 8) return;

    if (_ftw != W) {
        for (int x = 0; x < 512; ++x) {
            int xp = (x % 8) - 4;
            int yp = (x / 8) % 8 - 4;
            if (xp < 0) xp++;
            if (yp < 0) yp++;
            _fudgetable[x] = W * yp + xp;      // offset in pixels
        }
        _ftw = W;
    }

    if ((int)_scratch.size() < W * H * 4) _scratch.resize((size_t)W * H * 4);
    std::memcpy(_scratch.data(), cur.px.data(), (size_t)W * H * 4 * sizeof(float));

    for (int y = 4; y < H - 4; ++y) {
        for (int x = 0; x < W; ++x) {
            if (amount < 1.f && xrand(_rng) >= amount) continue;
            int idx = y * W + x + _fudgetable[xrandu(_rng) & 511];
            const float* s = &_scratch[(size_t)idx * 4];
            float* d = cur.at(x, y);
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        }
    }
}

// ---------------------------------------------------------------------------
// Grain — from e_grain.cpp (BSD-3). Non-black pixels only: where the noise
// threshold passes, the grain color is the pixel scaled by a random 0..1
// value; elsewhere the grain color is black. Blend modes replace/add/50-50
// match the original's. Static mode keeps a per-pixel (noise, threshold)
// field, rebuilt on resize.
void GrainEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                         const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;

    const bool isStatic = staticGrain > 0.5f;
    if (isStatic && (_dw != W || _dh != H)) {
        _depth.resize((size_t)W * H * 2);
        for (size_t i = 0; i < _depth.size(); i += 2) {
            _depth[i]     = xrand(_rng);       // noise value
            _depth[i + 1] = xrand(_rng);       // threshold
        }
        _dw = W; _dh = H;
    }

    float amt = amount;
    if (a.beat) amt = std::min(1.f, amt + onBeatBoost);
    const int bm = (int)blend;

    const int n = W * H;
    float* p = cur.px.data();
    const float* q = _depth.data();
    for (int i = 0; i < n; ++i, p += 4, q += 2) {
        if (p[0] <= 0.f && p[1] <= 0.f && p[2] <= 0.f) continue;   // skip black
        float noise, thresh;
        if (isStatic) { noise = q[0]; thresh = q[1]; }
        else          { thresh = xrand(_rng); noise = xrand(_rng); }
        float cr = 0.f, cg = 0.f, cb = 0.f;
        if (thresh < amt) {
            cr = std::min(1.f, p[0] * noise);
            cg = std::min(1.f, p[1] * noise);
            cb = std::min(1.f, p[2] * noise);
        }
        if (bm == 1) {              // additive
            p[0] = std::min(1.f, p[0] + cr);
            p[1] = std::min(1.f, p[1] + cg);
            p[2] = std::min(1.f, p[2] + cb);
        } else if (bm == 2) {       // 50/50
            p[0] = (p[0] + cr) * 0.5f;
            p[1] = (p[1] + cg) * 0.5f;
            p[2] = (p[2] + cb) * 0.5f;
        } else {                    // replace
            p[0] = cr; p[1] = cg; p[2] = cb;
        }
    }
}

// ---------------------------------------------------------------------------
// Interleave — from e_interleave.cpp (BSD-3). Row bands of `y` pixels
// alternate filled/unfilled; unfilled rows get column bands of `x` pixels.
// Edge-correction offsets ((w % stride)/2) keep the pattern centered, and the
// on-beat strides ease back via the same xy_lerp recurrence as the original.
void InterleaveEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                              const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;

    double lerpK = ((double)duration + 512.0 - 64.0) / 512.0;
    _curX = _curX * lerpK + (double)x * (1.0 - lerpK);
    _curY = _curY * lerpK + (double)y * (1.0 - lerpK);
    if (a.beat && onBeat > 0.5f) {
        _curX = (double)onBeatX;
        _curY = (double)onBeatY;
    }

    const int strideX = (int)_curX;
    const int strideY = (int)_curY;
    if (strideX < 0 || strideY < 0) return;

    const int bm = (int)blend;
    auto fillPx = [&](float* d) {
        if (bm == 1) {              // additive
            d[0] = std::min(1.f, d[0] + colR);
            d[1] = std::min(1.f, d[1] + colG);
            d[2] = std::min(1.f, d[2] + colB);
        } else if (bm == 2) {       // 50/50
            d[0] = (d[0] + colR) * 0.5f;
            d[1] = (d[1] + colG) * 0.5f;
            d[2] = (d[2] + colB) * 0.5f;
        } else {                    // replace
            d[0] = colR; d[1] = colG; d[2] = colB;
        }
    };

    bool fillY = true;
    int yCount = (strideY > 0) ? (H % strideY) / 2 : 0;
    const int xEdge = (strideX > 0) ? (W % strideX) / 2 : 0;

    for (int yy = 0; yy < H; ++yy) {
        if (strideY && ++yCount >= strideY) { fillY = !fillY; yCount = 0; }
        if (strideY && fillY) {
            for (int xx = 0; xx < W; ++xx) fillPx(cur.at(xx, yy));
        } else if (strideX) {
            bool fillX = true;
            int remaining = W, xx = 0;
            int edge = xEdge;
            while (remaining > 0) {
                int span = std::min(remaining, strideX - edge);
                edge = 0;
                if (fillX)
                    for (int k = 0; k < span; ++k) fillPx(cur.at(xx + k, yy));
                xx += span;
                remaining -= span;
                fillX = !fillX;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Interferences — from e_interferences.cpp (BSD-3). N copies of the frame,
// offset on a circle at the current rotation, summed with per-layer alpha and
// clamped. When separate_rgb is on and layers is a multiple of 3, layer i
// feeds only channel i%3 (the original's chromatic-ghosting mode). A beat
// resets the sin() envelope that sweeps the on-beat values back to base.
void InterferencesEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                                 const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    const int n = std::clamp((int)layers, 0, 8);
    if (W == 0 || H == 0 || n == 0) return;

    if (onBeat > 0.5f && a.beat && _fadeout >= kPi) _fadeout = 0.f;
    const float s = std::sin(_fadeout);

    const float dist    = distance + (onBeatDistance - distance) * s;
    const float alph    = alpha    + (onBeatAlpha    - alpha)    * s;
    const float rotStep = rotation + (onBeatRotation - rotation) * s;

    int xo[8], yo[8];
    float angle = _curRotation / 255.f * 2.f * kPi;
    const float angleStep = 2.f * kPi / n;
    for (int i = 0; i < n; ++i) {
        xo[i] = (int)(std::cos(angle) * dist);
        yo[i] = (int)(std::sin(angle) * dist);
        angle += angleStep;
    }

    if ((int)_scratch.size() < W * H * 4) _scratch.resize((size_t)W * H * 4);
    std::memcpy(_scratch.data(), cur.px.data(), (size_t)W * H * 4 * sizeof(float));

    const bool sepRGB = separateRGB > 0.5f && (n % 3 == 0);
    const int bm = (int)blend;

    for (int yy = 0; yy < H; ++yy) {
        for (int xx = 0; xx < W; ++xx) {
            float r = 0.f, g = 0.f, b = 0.f;
            for (int i = 0; i < n; ++i) {
                int sx = xx - xo[i], sy = yy - yo[i];
                if (sx < 0 || sx >= W || sy < 0 || sy >= H) continue;
                const float* p = &_scratch[((size_t)sy * W + sx) * 4];
                if (sepRGB) {
                    switch (i % 3) {
                        case 0: r += p[0] * alph; break;
                        case 1: g += p[1] * alph; break;
                        default: b += p[2] * alph; break;
                    }
                } else {
                    r += p[0] * alph; g += p[1] * alph; b += p[2] * alph;
                }
            }
            r = std::min(1.f, r); g = std::min(1.f, g); b = std::min(1.f, b);
            float* d = cur.at(xx, yy);
            const float* o = &_scratch[((size_t)yy * W + xx) * 4];
            if (bm == 1) {              // additive with original
                d[0] = std::min(1.f, o[0] + r);
                d[1] = std::min(1.f, o[1] + g);
                d[2] = std::min(1.f, o[2] + b);
            } else if (bm == 2) {       // 50/50 with original
                d[0] = (o[0] + r) * 0.5f;
                d[1] = (o[1] + g) * 0.5f;
                d[2] = (o[2] + b) * 0.5f;
            } else {                    // replace
                d[0] = r; d[1] = g; d[2] = b;
            }
        }
    }

    _curRotation += rotStep;
    if (_curRotation > 255.f)  _curRotation -= 255.f;
    if (_curRotation < -255.f) _curRotation += 255.f;

    _fadeout += onBeatSpeed;
    if (_fadeout > kPi) _fadeout = kPi;
}

// ---------------------------------------------------------------------------
// Roto Blitter — from e_rotoblitter.cpp (BSD-3). Zoom+rotate blit with a
// tiling (wrapping) source, sampling the previous frame (feedback, like
// FeedbackWarp). On-beat direction reversal eases current_rotation between +1
// and -1 at 1/(1 + reverse_speed*4) per frame; on-beat zoom kicks then eases
// back (we ease with a smooth lerp instead of the original's ±3/256 steps).
// Always bilinear (the original's non-bilinear mode is dropped).
bool RotoBlitterEffect::isGpu() const { return gpu::available(); }

void RotoBlitterEffect::render(Framebuffer& cur, const Framebuffer& /*prevFrame*/,
                               const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;

    if (a.beat && onBeatReverse > 0.5f) _dir = -_dir;
    if (onBeatReverse <= 0.5f) _dir = 1;

    _curRot += (1.f / (1.f + reverseSpeed * 4.f)) * ((float)_dir - _curRot);
    if (_dir > 0 && _curRot > (float)_dir) _curRot = (float)_dir;
    if (_dir < 0 && _curRot < (float)_dir) _curRot = (float)_dir;

    if (a.beat && onBeatZoomEnable > 0.5f) _curZoom = onBeatZoom;
    _curZoom += (zoom - _curZoom) * 0.05f;

    const float theta = rotate * _curRot * kPi / 180.f;
    const float ca = std::cos(theta) * _curZoom;
    const float sa = std::sin(theta) * _curZoom;
    const float cx = (W - 1) * 0.5f, cy = (H - 1) * 0.5f;
    const bool doBlend = blend > 0.5f;

    if (gpu::available()) { gpu::rotoBlit(cur, ca, sa, doBlend); return; }

    // CPU path. AVS roto-blitter transforms the CURRENT frame (fb -> fbout),
    // not the previous one: snapshot cur and resample from the snapshot.
    _snap.w = W; _snap.h = H;
    _snap.px = cur.px;
    const Framebuffer& prev = _snap;

    float px[4];
    for (int y = 0; y < H; ++y) {
        float dy = y - cy;
        for (int x = 0; x < W; ++x) {
            float dx = x - cx;
            float sx = cx + dx * ca - dy * sa;
            float sy = cy + dx * sa + dy * ca;
            sampleWrap(prev, sx, sy, px);
            float* d = cur.at(x, y);
            if (doBlend) {              // 50/50 with existing frame
                d[0] = (d[0] + px[0]) * 0.5f;
                d[1] = (d[1] + px[1]) * 0.5f;
                d[2] = (d[2] + px[2]) * 0.5f;
            } else {
                d[0] = px[0]; d[1] = px[1]; d[2] = px[2];
            }
            d[3] = 1.f;
        }
    }
    quantize8(cur.px);            // 8-bit truncation, as AVS (Framebuffer.h)
}

// ---------------------------------------------------------------------------
// Add Borders — from e_addborders.cpp (BSD-3; original APE by Goebish).
// Solid border, width = size percent of each dimension (min 1px). Our on-beat
// pulse widens it temporarily and decays multiplicatively.
void AddBordersEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                              const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;

    if (a.beat && onBeatPulse > 0.f) _pulse = onBeatPulse;
    float pct = std::min(50.f, size + _pulse);
    _pulse *= pulseDecay;

    int bh = std::max(1, (int)(H * pct / 100.f));
    int bw = std::max(1, (int)(W * pct / 100.f));
    bh = std::min(bh, H / 2);
    bw = std::min(bw, W / 2);

    auto setPx = [&](int xx, int yy) {
        float* d = cur.at(xx, yy);
        d[0] = colR; d[1] = colG; d[2] = colB; d[3] = 1.f;
    };
    for (int i = 0; i < bh; ++i)
        for (int k = 0; k < W; ++k) { setPx(k, i); setPx(k, H - 1 - i); }
    for (int i = 0; i < bw; ++i)
        for (int k = 0; k < H; ++k) { setPx(i, k); setPx(W - 1 - i, k); }
}

// ---------------------------------------------------------------------------
// Movement — from e_movement.cpp / e_movement.h (BSD-3). The original ran
// tiny scripts to build a dest→source lookup table; we evaluate the classic
// preset formulas (from the Movement_Info::effects table) directly per pixel.
// Polar convention matches AVS: d normalized 0..1 by the half-diagonal,
// r in radians. Bilinear sampling; wrap tiles the source, else it clamps.
bool MovementEffect::isGpu() const { return gpu::available(); }

void MovementEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                            const VizFrame& /*a*/, const EffectContext& ctx)
{
    const int W = cur.w, H = cur.h;
    if (W < 2 || H < 2) return;

    const int m = std::clamp((int)mode, 0, 8);
    const bool doWrap  = wrap > 0.5f;
    const bool doBlend = blend > 0.5f;

    if (gpu::available()) { gpu::movement(cur, m, doWrap, doBlend, (int)ctx.frame); return; }

    // CPU path: in-place spatial transform — snapshot the current frame, then
    // resample (the original transformed framebuffer → fbout of the same frame).
    _scratch.w = W; _scratch.h = H;
    _scratch.px = cur.px;

    const float cx = (W - 1) * 0.5f, cy = (H - 1) * 0.5f;
    const float maxd = std::sqrt((float)W * W + (float)H * H) * 0.5f;

    float px[4];
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float sx, sy;
            bool nearest = false;

            switch (m) {
                case 0: {   // Slight Fuzzify: ±1px random jitter
                    sx = (float)x + (float)((int)(xrandu(_rng) % 3u) - 1);
                    sy = (float)y + (float)((int)(xrandu(_rng) % 3u) - 1);
                    nearest = true;
                    break;
                }
                case 1: {   // Shift Rotate Left: x = x + 1/32 (of -1..1 span)
                    sx = (float)x + (float)W / 64.f;
                    sy = (float)y;
                    break;
                }
                default: {  // polar presets
                    float dx = x - cx, dy = y - cy;
                    float d = std::sqrt(dx * dx + dy * dy) / maxd;
                    float r = std::atan2(dy, dx);
                    switch (m) {
                        case 2:   // Big Swirl Out
                            r += 0.1f - 0.2f * d;
                            d *= 0.96f;
                            break;
                        case 3:   // Medium Swirl
                            d *= 0.99f * (1.f - std::sin(r - kPi * 0.5f) / 32.f);
                            r += 0.03f * std::sin(d * kPi * 4.f);
                            break;
                        case 4:   // Sunburster
                            d *= 0.94f + std::cos((r - kPi * 0.5f) * 32.f) * 0.06f;
                            break;
                        case 5:   // Swirl To Center
                            d *= 1.01f + std::cos((r - kPi * 0.5f) * 4.f) * 0.04f;
                            r += 0.03f * std::sin(d * kPi * 4.f);
                            break;
                        case 6: { // Bubbling Outward
                            float t = std::sin(d * kPi);
                            d -= 8.f * t * t * t * t * t / maxd;
                            break;
                        }
                        case 7:   // Tunneling
                            r += 0.04f;
                            d *= 0.96f + std::cos(d * kPi) * 0.05f;
                            break;
                        default:  // 8: Bleedin'
                        {
                            float t = std::cos(d * kPi);
                            r += 0.07f * t;
                            d *= 0.98f + t * 0.10f;
                            break;
                        }
                    }
                    sx = cx + std::cos(r) * d * maxd;
                    sy = cy + std::sin(r) * d * maxd;
                    break;
                }
            }

            if (nearest) {
                int ix = (int)sx, iy = (int)sy;
                if (doWrap) {
                    ix = ((ix % W) + W) % W;
                    iy = ((iy % H) + H) % H;
                } else {
                    ix = std::clamp(ix, 0, W - 1);
                    iy = std::clamp(iy, 0, H - 1);
                }
                const float* s = _scratch.at(ix, iy);
                px[0] = s[0]; px[1] = s[1]; px[2] = s[2]; px[3] = 1.f;
            } else if (doWrap) {
                sampleWrap(_scratch, sx, sy, px);
            } else {
                _scratch.sample(sx, sy, px);
            }

            float* d = cur.at(x, y);
            if (doBlend) {              // 50/50 with the original frame
                const float* o = _scratch.at(x, y);
                d[0] = (o[0] + px[0]) * 0.5f;
                d[1] = (o[1] + px[1]) * 0.5f;
                d[2] = (o[2] + px[2]) * 0.5f;
            } else {
                d[0] = px[0]; d[1] = px[1]; d[2] = px[2];
            }
            d[3] = 1.f;
        }
    }
    quantize8(cur.px);            // 8-bit truncation, as AVS (Framebuffer.h)
}

} // namespace viz
