//
// GpuFx.mm — see GpuFx.h.
//
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "../GpuFx.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unordered_map>

namespace viz { namespace gpu {

// ---------------------------------------------------------------------------
//  Kernel library (compiled from source at first use — no build-system deps)
// ---------------------------------------------------------------------------
static const char* kKernels = R"MSL(
#include <metal_stdlib>
using namespace metal;

kernel void fx_threshold(texture2d<float, access::read> src [[texture(0)]],
                         texture2d<float, access::write> dst [[texture(1)]],
                         constant float* p [[buffer(0)]],
                         uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    float4 c = src.read(g);
    float lum = dot(c.rgb, float3(0.299, 0.587, 0.114));
    float t = p[0];
    float k = lum <= t ? 0.0 : (lum - t) / max(1.0 - t, 1e-4);
    dst.write(float4(c.rgb * k, 1.0), g);
}

// p[0] = sigma, p[1] = taps, dir = (p[2], p[3])
kernel void fx_blur(texture2d<float, access::read> src [[texture(0)]],
                    texture2d<float, access::write> dst [[texture(1)]],
                    constant float* p [[buffer(0)]],
                    uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    int W = (int)src.get_width(), H = (int)src.get_height();
    float sigma = max(p[0], 0.5f);
    int taps = clamp((int)p[1], 1, 63);
    int2 dir = int2((int)p[2], (int)p[3]);
    float3 acc = 0.0;
    float wsum = 0.0;
    for (int i = -taps; i <= taps; ++i) {
        int2 q = int2(g) + dir * i;
        q.x = clamp(q.x, 0, W - 1);
        q.y = clamp(q.y, 0, H - 1);
        float w = exp(-0.5f * (float)(i * i) / (sigma * sigma));
        acc += src.read(uint2(q)).rgb * w;
        wsum += w;
    }
    dst.write(float4(acc / wsum, 1.0), g);
}

// dst = base + glow * p[0], soft-clamped
kernel void fx_bloom_combine(texture2d<float, access::read> base [[texture(0)]],
                             texture2d<float, access::read> glow [[texture(1)]],
                             texture2d<float, access::write> dst [[texture(2)]],
                             constant float* p [[buffer(0)]],
                             uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    float3 c = base.read(g).rgb + glow.read(g).rgb * p[0];
    c = 1.0 - exp(-c);                 // soft rolloff instead of hard clip
    dst.write(float4(c, 1.0), g);
}

// p = [segments, angle, zoom]
kernel void fx_kaleido(texture2d<float, access::sample> src [[texture(0)]],
                       texture2d<float, access::write> dst [[texture(1)]],
                       constant float* p [[buffer(0)]],
                       uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler smp(address::clamp_to_edge, filter::linear);
    float W = dst.get_width(), H = dst.get_height();
    float2 c = float2(W, H) * 0.5;
    float2 d = (float2(g) - c) / c.y;           // aspect-true, unit half-height
    float r = length(d);
    float seg = 6.2831853 / max(p[0], 2.0f);
    float a = atan2(d.y, d.x) + p[1];
    a = fmod(a, seg); if (a < 0.0) a += seg;
    a = abs(a - seg * 0.5);                     // mirror fold within segment
    float2 s = float2(cos(a), sin(a)) * r * p[2];
    float2 uv = (s * c.y + c) / float2(W, H);
    dst.write(float4(src.sample(smp, uv).rgb, 1.0), g);
}

// p = [r0, r1, r2, r3, width, strength]  (radii in unit half-height, <0 = off)
kernel void fx_shockwave(texture2d<float, access::sample> src [[texture(0)]],
                         texture2d<float, access::write> dst [[texture(1)]],
                         constant float* p [[buffer(0)]],
                         uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler smp(address::clamp_to_edge, filter::linear);
    float W = dst.get_width(), H = dst.get_height();
    float2 c = float2(W, H) * 0.5;
    float2 d = (float2(g) - c) / (H * 0.5);
    float dist = length(d);
    float2 dir = dist > 1e-4 ? d / dist : float2(0.0);
    float disp = 0.0, glow = 0.0;
    for (int i = 0; i < 4; ++i) {
        if (p[i] < 0.0) continue;
        float x = (dist - p[i]) / max(p[4], 1e-3f);
        float band = exp(-x * x);
        disp += band * p[5] * (dist - p[i] < 0.0 ? -1.0 : 1.0);
        glow += band;
    }
    float2 uv = ((d - dir * disp) * (H * 0.5) + c) / float2(W, H);
    float3 col = src.sample(smp, uv).rgb * (1.0 + glow * 0.25);
    dst.write(float4(clamp(col, 0.0, 4.0), 1.0), g);
}

// p = [amount(0..1), seed, blockPx, tearPx]
kernel void fx_glitch(texture2d<float, access::sample> src [[texture(0)]],
                      texture2d<float, access::write> dst [[texture(1)]],
                      constant float* p [[buffer(0)]],
                      uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler smp(address::clamp_to_edge, filter::linear);
    float2 sz = float2(dst.get_width(), dst.get_height());
    float amount = p[0], seed = p[1];
    float band = floor((float)g.y / max(p[2], 2.0f));
    auto h = [](float x, float y) {
        float s = sin(x * 127.1f + y * 311.7f) * 43758.5453f;
        return s - floor(s);
    };
    float active = h(band, seed) < amount * 0.6f ? 1.0f : 0.0f;
    float shift = (h(band + 17.0f, seed) - 0.5f) * 2.0f * amount * 0.25f * active;
    float tear = p[3] * amount * active / sz.x;
    float2 uv = (float2(g) + 0.5) / sz;
    uv.x = fract(uv.x + shift);
    float r = src.sample(smp, float2(fract(uv.x + tear), uv.y)).r;
    float gg = src.sample(smp, uv).g;
    float b = src.sample(smp, float2(fract(uv.x - tear), uv.y)).b;
    float3 col = float3(r, gg, b);
    if (active > 0.5f && h(band + 41.0f, seed) < amount * 0.4f)
        col = floor(col * 6.0f) / 6.0f;              // momentary posterize
    dst.write(float4(col, 1.0), g);
}

// p = [intensity, tintR, tintG, tintB]: dst = base + streak * intensity * tint
kernel void fx_streak_combine(texture2d<float, access::read> base [[texture(0)]],
                              texture2d<float, access::read> streak [[texture(1)]],
                              texture2d<float, access::write> dst [[texture(2)]],
                              constant float* p [[buffer(0)]],
                              uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    float3 c = base.read(g).rgb
             + streak.read(g).rgb * p[0] * float3(p[1], p[2], p[3]);
    dst.write(float4(1.0 - exp(-c), 1.0), g);
}

// p = [strength]  (>0 barrel/fisheye, <0 pincushion; aspect-true)
kernel void fx_lens(texture2d<float, access::sample> src [[texture(0)]],
                    texture2d<float, access::write> dst [[texture(1)]],
                    constant float* p [[buffer(0)]],
                    uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler smp(address::clamp_to_edge, filter::linear);
    float W = dst.get_width(), H = dst.get_height();
    float2 c = float2(W, H) * 0.5;
    float2 d = (float2(g) - c) / (H * 0.5);
    float r = length(d);
    // remap radius: fisheye compresses edges (sample closer to center)
    float k = 1.0 + p[0] * r * r;
    float2 uv = ((d / k) * (H * 0.5) + c) / float2(W, H);
    dst.write(float4(src.sample(smp, uv).rgb, 1.0), g);
}

// p = [amount(0..1), taps]  (zoom blur toward center)
kernel void fx_radialblur(texture2d<float, access::sample> src [[texture(0)]],
                          texture2d<float, access::write> dst [[texture(1)]],
                          constant float* p [[buffer(0)]],
                          uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler smp(address::clamp_to_edge, filter::linear);
    float2 sz = float2(dst.get_width(), dst.get_height());
    float2 uv = (float2(g) + 0.5) / sz;
    float2 cuv = float2(0.5);
    int taps = clamp((int)p[1], 4, 32);
    float3 acc = 0.0;
    for (int i = 0; i < taps; ++i) {
        float t = p[0] * 0.35f * (float)i / (float)taps;
        acc += src.sample(smp, mix(uv, cuv, t)).rgb;
    }
    dst.write(float4(acc / (float)taps, 1.0), g);
}

// p = [glow, keepSource, r, g, b]  (sobel edges → tinted neon, added or solo)
kernel void fx_edges(texture2d<float, access::read> src [[texture(0)]],
                     texture2d<float, access::write> dst [[texture(1)]],
                     constant float* p [[buffer(0)]],
                     uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    int W = (int)dst.get_width(), H = (int)dst.get_height();
    auto lum = [&](int x, int y) {
        x = clamp(x, 0, W - 1); y = clamp(y, 0, H - 1);
        float3 c = src.read(uint2(x, y)).rgb;
        return dot(c, float3(0.299, 0.587, 0.114));
    };
    int x = (int)g.x, y = (int)g.y;
    float gx = lum(x+1,y-1) + 2.0*lum(x+1,y) + lum(x+1,y+1)
             - lum(x-1,y-1) - 2.0*lum(x-1,y) - lum(x-1,y+1);
    float gy = lum(x-1,y+1) + 2.0*lum(x,y+1) + lum(x+1,y+1)
             - lum(x-1,y-1) - 2.0*lum(x,y-1) - lum(x+1,y-1);
    float e = clamp(sqrt(gx*gx + gy*gy) * p[0], 0.0f, 1.0f);
    float3 edge = float3(p[2], p[3], p[4]) * e;
    float3 base = src.read(g).rgb * p[1];
    dst.write(float4(1.0 - exp(-(base + edge)), 1.0), g);
}

// p = [shadowR,G,B, highlightR,G,B, mix]  (luminance → two-color gradient)
kernel void fx_duotone(texture2d<float, access::read> src [[texture(0)]],
                       texture2d<float, access::write> dst [[texture(1)]],
                       constant float* p [[buffer(0)]],
                       uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    float3 c = src.read(g).rgb;
    float lum = dot(c, float3(0.299, 0.587, 0.114));
    float3 duo = mix(float3(p[0], p[1], p[2]), float3(p[3], p[4], p[5]),
                     smoothstep(0.0f, 1.0f, lum));
    dst.write(float4(mix(c, duo, p[6]), 1.0), g);
}

// p = [amountPx, scale, t]  (animated value-noise displacement field)
static inline float sh_hash(float x, float y) {
    float s = sin(x * 127.1f + y * 311.7f) * 43758.5453f;
    return s - floor(s);
}
static inline float sh_noise(float x, float y) {
    float ix = floor(x), iy = floor(y), fx = x - ix, fy = y - iy;
    float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy);
    float a = sh_hash(ix, iy), b = sh_hash(ix + 1.0f, iy);
    float c = sh_hash(ix, iy + 1.0f), d = sh_hash(ix + 1.0f, iy + 1.0f);
    return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}
kernel void fx_shimmer(texture2d<float, access::sample> src [[texture(0)]],
                       texture2d<float, access::write> dst [[texture(1)]],
                       constant float* p [[buffer(0)]],
                       uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler smp(address::clamp_to_edge, filter::linear);
    float2 sz = float2(dst.get_width(), dst.get_height());
    float2 uv = (float2(g) + 0.5) / sz;
    float sx = uv.x * p[1], sy = uv.y * p[1] * (sz.y / sz.x);
    float nx = sh_noise(sx + p[2], sy) - 0.5f;
    float ny = sh_noise(sx + 37.7f, sy + p[2] * 1.31f) - 0.5f;
    float2 off = float2(nx, ny) * 2.0f * p[0] / sz;
    dst.write(float4(src.sample(smp, uv + off).rgb, 1.0), g);
}

// p = [curvature, scanStrength, maskStrength, corner]
kernel void fx_crt(texture2d<float, access::sample> src [[texture(0)]],
                   texture2d<float, access::write> dst [[texture(1)]],
                   constant float* p [[buffer(0)]],
                   uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler smp(address::clamp_to_edge, filter::linear);
    float2 sz = float2(dst.get_width(), dst.get_height());
    float2 uv = (float2(g) + 0.5) / sz * 2.0 - 1.0;      // -1..1
    float r2 = dot(uv, uv);
    float2 cuv = uv * (1.0 + p[0] * r2);                  // barrel curvature
    float2 suv = (cuv + 1.0) * 0.5;
    float3 col = (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0)
               ? float3(0.0) : src.sample(smp, suv).rgb;
    float scan = 1.0 - p[1] * (0.5 + 0.5 * sin(suv.y * sz.y * 3.14159f));
    int m = (int)g.x % 3;                                 // phosphor triad
    float3 mask = float3(m == 0 ? 1.0 : 1.0 - p[2],
                         m == 1 ? 1.0 : 1.0 - p[2],
                         m == 2 ? 1.0 : 1.0 - p[2]);
    float corner = 1.0 - p[3] * smoothstep(0.6f, 1.6f, r2);
    dst.write(float4(col * scan * mask * corner, 1.0), g);
}

// p = [exposure, invGamma, saturation]
kernel void fx_tonemap(texture2d<float, access::read> src [[texture(0)]],
                       texture2d<float, access::write> dst [[texture(1)]],
                       constant float* p [[buffer(0)]],
                       uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    float3 c = src.read(g).rgb;
    c = 1.0 - exp(-c * p[0]);                  // filmic-ish exposure rolloff
    c = pow(max(c, 0.0), float3(p[1]));        // gamma
    float lum = dot(c, float3(0.299, 0.587, 0.114));
    c = mix(float3(lum), c, p[2]);             // saturation
    dst.write(float4(clamp(c, 0.0, 1.0), 1.0), g);
}

// p = [inner, outer, strength]  (aspect-true radius, 1 = half-height edge)
kernel void fx_vignette(texture2d<float, access::read> src [[texture(0)]],
                        texture2d<float, access::write> dst [[texture(1)]],
                        constant float* p [[buffer(0)]],
                        uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    float W = dst.get_width(), H = dst.get_height();
    float2 c = float2(W, H) * 0.5;
    float2 d = (float2(g) - c) / c.y;
    float t = smoothstep(p[0], max(p[1], p[0] + 1e-3f), length(d));
    float k = 1.0 - t * p[2];
    float3 col = src.read(g).rgb * k;
    dst.write(float4(col, 1.0), g);
}

// p = [dx, dy] in pixels
// Trails against the previous frame: p[0] persistence, p[1] mode
// (0 crossfade, 1 phosphor max, 2 fade: the previous frame, dimmed).
kernel void fx_trails(texture2d<float, access::read> cur [[texture(0)]],
                      texture2d<float, access::read> prev [[texture(1)]],
                      texture2d<float, access::write> dst [[texture(2)]],
                      constant float* p [[buffer(0)]],
                      uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    float4 c = cur.read(g);
    float3 q = prev.read(g).rgb * p[0];
    if (p[1] > 1.5)      dst.write(float4(q, 1.0), g);
    else if (p[1] > 0.5) dst.write(float4(max(c.rgb, q), c.a), g);
    else                 dst.write(float4(c.rgb * (1.0 - p[0]) + q, c.a), g);
}

kernel void fx_rgbsplit(texture2d<float, access::sample> src [[texture(0)]],
                        texture2d<float, access::write> dst [[texture(1)]],
                        constant float* p [[buffer(0)]],
                        uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler smp(address::clamp_to_edge, filter::linear);
    float2 sz = float2(dst.get_width(), dst.get_height());
    float2 uv = (float2(g) + 0.5) / sz;
    float2 off = float2(p[0], p[1]) / sz;
    float r = src.sample(smp, uv + off).r;
    float ga = src.sample(smp, uv).g;
    float b = src.sample(smp, uv - off).b;
    dst.write(float4(r, ga, b, 1.0), g);
}
)MSL";

// ---------------------------------------------------------------------------
//  Context
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  Classic ports (see GpuFx.h) — appended to the same library.
// ---------------------------------------------------------------------------
static const char* kClassicKernels = R"MSL(
#include <metal_stdlib>
using namespace metal;

// Bilinear at PIXEL coords, matching Framebuffer::sample (clamp) and the
// CPU sampleWrap (tiling): texel centers sit at +0.5.
static inline float4 wv_sample(texture2d<float, access::sample> t, float2 s, bool wrap)
{
    constexpr sampler sc(coord::normalized, address::clamp_to_edge, filter::linear);
    constexpr sampler sw(coord::normalized, address::repeat, filter::linear);
    float2 uv = (s + 0.5) / float2(t.get_width(), t.get_height());
    if (wrap) return t.sample(sw, uv);
    return t.sample(sc, uv);
}

static inline uint wv_hash(uint x, uint y, uint seed)
{
    uint h = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return h;
}

// 8-bit truncation (see Framebuffer.h q8): AVS's integer framebuffer floored
// every resample/blend, and feedback presets depend on that energy loss.
static inline float3 wv_q8(float3 v)
{
    return floor(clamp(v, 0.0f, 1.0f) * 255.0f) * (1.0f / 255.0f);
}

// Movement / Roto Blitter / Feedback Warp share one kernel.
// p = [mode, wrap, blend, seed, ca, sa, decay]
//   0..8 : Movement built-ins over the frame itself
//   9    : affine, tiling source (Roto Blitter)
//   10   : affine, clamped, * decay, source = previous frame (Feedback Warp)
kernel void fx_warp(texture2d<float, access::sample> src [[texture(0)]],
                    texture2d<float, access::write> dst [[texture(1)]],
                    constant float* p [[buffer(0)]],
                    uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    const int W = (int)dst.get_width(), H = (int)dst.get_height();
    const int mode = (int)p[0];
    bool wrap = p[1] > 0.5;
    const bool blend = p[2] > 0.5;
    const float cx = (W - 1) * 0.5f, cy = (H - 1) * 0.5f;
    const float x = (float)g.x, y = (float)g.y;
    float4 outc;
    if (mode == 0) {                             // Slight Fuzzify: +-1px, nearest
        uint h = wv_hash(g.x, g.y, (uint)p[3]);
        int ix = (int)g.x + (int)(h % 3u) - 1;
        int iy = (int)g.y + (int)((h >> 8) % 3u) - 1;
        if (wrap) { ix = ((ix % W) + W) % W; iy = ((iy % H) + H) % H; }
        else      { ix = clamp(ix, 0, W - 1);   iy = clamp(iy, 0, H - 1); }
        outc = src.read(uint2(ix, iy));
    } else {
        float sx, sy;
        if (mode == 1) {                         // Shift Rotate Left
            sx = x + (float)W / 64.0f; sy = y;
        } else if (mode >= 9) {                  // affine (roto / feedback)
            float dx = x - cx, dy = y - cy;
            sx = cx + dx * p[4] - dy * p[5];
            sy = cy + dx * p[5] + dy * p[4];
            wrap = (mode == 9);
        } else {                                 // polar presets
            const float PI = 3.14159265358979f;
            float maxd = sqrt((float)W * W + (float)H * H) * 0.5f;
            float dx = x - cx, dy = y - cy;
            float d = sqrt(dx * dx + dy * dy) / maxd;
            float r = atan2(dy, dx);
            switch (mode) {
                case 2: r += 0.1f - 0.2f * d; d *= 0.96f; break;
                case 3: d *= 0.99f * (1.0f - sin(r - PI * 0.5f) / 32.0f);
                        r += 0.03f * sin(d * PI * 4.0f); break;
                case 4: d *= 0.94f + cos((r - PI * 0.5f) * 32.0f) * 0.06f; break;
                case 5: d *= 1.01f + cos((r - PI * 0.5f) * 4.0f) * 0.04f;
                        r += 0.03f * sin(d * PI * 4.0f); break;
                case 6: { float t = sin(d * PI); d -= 8.0f * t * t * t * t * t / maxd; break; }
                case 7: r += 0.04f; d *= 0.96f + cos(d * PI) * 0.05f; break;
                default: { float t = cos(d * PI); r += 0.07f * t; d *= 0.98f + t * 0.10f; break; }
            }
            sx = cx + cos(r) * d * maxd;
            sy = cy + sin(r) * d * maxd;
        }
        outc = wv_sample(src, float2(sx, sy), wrap);
    }
    if (mode == 10) {                            // Feedback Warp (ours): stays float
        outc.rgb *= p[6];
    } else {
        if (blend) outc.rgb = (src.read(g).rgb + outc.rgb) * 0.5f;
        outc.rgb = wv_q8(outc.rgb);
    }
    dst.write(float4(outc.rgb, 1.0), g);
}

// Separable box blur. p = [R, dirx, diry, mix]; orig = the unblurred frame
// the final pass mixes against.
kernel void fx_boxblur(texture2d<float, access::read> src [[texture(0)]],
                       texture2d<float, access::read> orig [[texture(1)]],
                       texture2d<float, access::write> dst [[texture(2)]],
                       constant float* p [[buffer(0)]],
                       uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    const int W = (int)dst.get_width(), H = (int)dst.get_height();
    const int R = clamp((int)p[0], 0, 64);
    const int2 dir = int2((int)p[1], (int)p[2]);
    float3 acc = 0.0;
    for (int k = -R; k <= R; ++k) {
        int2 q = clamp(int2(g) + dir * k, int2(0), int2(W - 1, H - 1));
        acc += src.read(uint2(q)).rgb;
    }
    acc /= (float)(2 * R + 1);
    float3 o = orig.read(g).rgb;
    dst.write(float4(wv_q8(o + (acc - o) * p[3]), 1.0), g);
}

// Dynamic Movement per-pixel stage. p = [GX, GY, wrap, blend];
// grid = GX*GY triples (srcX, srcY, alpha), row-major.
kernel void fx_gridwarp(texture2d<float, access::sample> src [[texture(0)]],
                        texture2d<float, access::write> dst [[texture(1)]],
                        constant float* p [[buffer(0)]],
                        constant float* grid [[buffer(1)]],
                        uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    const int W = (int)dst.get_width(), H = (int)dst.get_height();
    const int GX = (int)p[0], GY = (int)p[1];
    const bool wrap = p[2] > 0.5, blend = p[3] > 0.5;
    float fy = (float)g.y / (float)(H - 1) * (float)(GY - 1);
    int gy0 = min((int)fy, GY - 2);
    float ty = fy - (float)gy0;
    float fx = (float)g.x / (float)(W - 1) * (float)(GX - 1);
    int gx0 = min((int)fx, GX - 2);
    float tx = fx - (float)gx0;
    constant float* v00 = grid + (gy0 * GX + gx0) * 3;
    constant float* v01 = grid + ((gy0 + 1) * GX + gx0) * 3;
    float3 top = mix(float3(v00[0], v00[1], v00[2]), float3(v00[3], v00[4], v00[5]), tx);
    float3 bot = mix(float3(v01[0], v01[1], v01[2]), float3(v01[3], v01[4], v01[5]), tx);
    float3 s = mix(top, bot, ty);                // (srcX, srcY, alpha)
    float3 c = wv_sample(src, s.xy, wrap).rgb;
    float3 o = src.read(g).rgb;
    float3 r = blend ? (o + (c - o) * s.z) : c;
    dst.write(float4(wv_q8(r), 1.0), g);
}

// Dynamic Shift: whole-frame offset by (sx, sy), black where the source is
// uncovered. p = [sx, sy, blend, alpha, bilinear]
kernel void fx_shift(texture2d<float, access::sample> src [[texture(0)]],
                     texture2d<float, access::write> dst [[texture(1)]],
                     constant float* p [[buffer(0)]],
                     uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    const int W = (int)dst.get_width(), H = (int)dst.get_height();
    float fx = (float)g.x - p[0], fy = (float)g.y - p[1];
    float3 c = 0.0;
    if (!(fx < 0.0f || fy < 0.0f || fx > (float)(W - 1) || fy > (float)(H - 1))) {
        if (p[4] > 0.5f) c = wv_sample(src, float2(fx, fy), false).rgb;
        else             c = src.read(uint2((int)fx, (int)fy)).rgb;
    }
    float3 o = src.read(g).rgb;
    float3 r = p[2] > 0.5f ? o + (c - o) * p[3] : c;
    dst.write(float4(wv_q8(r), 1.0), g);
}

// Dynamic Distance Modifier: radial remap through a per-radius scale table.
// p = [maxD, blend, bilinear]; table = maxD scale factors (buffer 1).
kernel void fx_distmod(texture2d<float, access::sample> src [[texture(0)]],
                       texture2d<float, access::write> dst [[texture(1)]],
                       constant float* p [[buffer(0)]],
                       constant float* table [[buffer(1)]],
                       uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    const int W = (int)dst.get_width(), H = (int)dst.get_height();
    const int maxD = (int)p[0];
    const float cx = (W - 1) * 0.5f, cy = (H - 1) * 0.5f;
    float dx = (float)g.x - cx, dy = (float)g.y - cy;
    float dist = sqrt(dx * dx + dy * dy);
    int di = min((int)dist, maxD - 1);
    float f = table[di];
    float fx = clamp(cx + dx * f, 0.0f, (float)(W - 1));
    float fy = clamp(cy + dy * f, 0.0f, (float)(H - 1));
    float3 c = p[2] > 0.5f ? wv_sample(src, float2(fx, fy), false).rgb
                           : src.read(uint2((int)fx, (int)fy)).rgb;
    float3 o = src.read(g).rgb;
    float3 r = p[1] > 0.5f ? (o + c) * 0.5f : c;
    dst.write(float4(wv_q8(r), 1.0), g);
}

// 5x5 convolution. p = [invScale, bias, absolute, wrap, mix, k0..k24].
// Edge handling is done by the sampler (nearest, clamp or repeat), which
// is much cheaper than per-tap integer clamp/modulo arithmetic.
kernel void fx_conv5(texture2d<float, access::sample> src [[texture(0)]],
                     texture2d<float, access::write> dst [[texture(1)]],
                     constant float* p [[buffer(0)]],
                     uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler sc(coord::normalized, address::clamp_to_edge, filter::nearest);
    constexpr sampler sw(coord::normalized, address::repeat, filter::nearest);
    const bool wrap = p[3] > 0.5;
    const float2 texel = 1.0 / float2(dst.get_width(), dst.get_height());
    const float2 base = (float2(g) + 0.5) * texel;
    float3 acc = 0.0;
    for (int ky = -2; ky <= 2; ++ky) {
        for (int kx = -2; kx <= 2; ++kx) {
            float w = p[5 + (ky + 2) * 5 + (kx + 2)];
            if (w == 0.0f) continue;
            float2 uv = base + float2(kx, ky) * texel;
            acc += w * (wrap ? src.sample(sw, uv).rgb : src.sample(sc, uv).rgb);
        }
    }
    float3 v = acc * p[0] + p[1];
    if (p[2] > 0.5f) v = abs(v);
    v = clamp(v, 0.0f, 1.0f);
    float3 d = src.read(g).rgb;
    dst.write(float4(wv_q8(d + (v - d) * p[4]), 1.0), g);
}

static inline float3 wv_blend3(int bm, float3 s, float3 d)
{
    if (bm == 1) return min(s + d, 1.0f);       // additive
    if (bm == 2) return (s + d) * 0.5f;         // fifty-fifty
    return s;                                   // replace
}

// Per-pixel color transforms (TransColor family + Color Map). p[0] = op:
//  0 Brightness  p1..3 channel factors, p4 blend (0 replace 1 add 2 50/50)
//  1 Invert
//  2 Fadeout     p1 speed, p2..4 target
//  3 Unique Tone p1..3 tone, p4 invert, p5 blend
//  4 Channel Shift p1..3 source channel index per destination channel
//  5 Colorfade   p1..12 offsets table [brightest-channel case][rgb]
//  6 Multiplier  p1 mode (0 inf-root, 7 inf-square), p2 factor
//  7 Color Reduction p1 levels
//  8 Color Clip  p1 mode, p2..4 in, p5..7 out, p8 squared radius
//  9 Color Map   lut = 256 rgb triplets (buffer 1), by Rec.601 luma
kernel void fx_colorop(texture2d<float, access::read> src [[texture(0)]],
                       texture2d<float, access::write> dst [[texture(1)]],
                       constant float* p [[buffer(0)]],
                       constant float* lut [[buffer(1)]],
                       uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    float3 c = src.read(g).rgb;
    float3 o = c;
    switch ((int)p[0]) {
        case 0: {
            float3 s = clamp(c * float3(p[1], p[2], p[3]), 0.0f, 1.0f);
            o = wv_blend3((int)p[4], s, c);
            break;
        }
        case 1: o = 1.0f - c; break;
        case 2: {
            float sp = p[1];
            float3 t = float3(p[2], p[3], p[4]);
            float3 inner = select(t, c - sp, c >= t + sp);
            o = select(inner, c + sp, c <= t - sp);
            break;
        }
        case 3: {
            float depth = max(c.r, max(c.g, c.b));
            if (p[4] > 0.5f) depth = 1.0f - depth;
            o = wv_blend3((int)p[5], depth * float3(p[1], p[2], p[3]), c);
            break;
        }
        case 4: {
            float v[3] = {c.r, c.g, c.b};
            o = float3(v[(int)p[1]], v[(int)p[2]], v[(int)p[3]]);
            break;
        }
        case 5: {
            float gb = c.g - c.b, br = c.b - c.r;
            int s;
            if (gb > 0.0f && gb > -br)       s = 0;
            else if (br < 0.0f && gb < -br)  s = 1;
            else if (gb < 0.0f && br > 0.0f) s = 2;
            else                             s = 3;
            o = clamp(c + float3(p[1 + s * 3], p[2 + s * 3], p[3 + s * 3]), 0.0f, 1.0f);
            break;
        }
        case 6: {
            int m = (int)p[1];
            if (m == 0)      { float hi = 254.5f / 255.0f; o = (c.r < hi || c.g < hi || c.b < hi) ? float3(0.0f) : c; }
            else if (m >= 7) { float lo = 0.5f / 255.0f;   o = (c.r > lo || c.g > lo || c.b > lo) ? float3(1.0f) : c; }
            else             o = min(c * p[2], 1.0f);
            break;
        }
        case 7: {
            float lv = p[1];
            float3 q = floor(clamp(c, 0.0f, 1.0f) * lv);
            o = min(q, lv - 1.0f) / lv;
            break;
        }
        case 8: {
            int m = (int)p[1];
            float3 in = float3(p[2], p[3], p[4]);
            bool hit;
            if (m == 0)      hit = all(c <= in);
            else if (m == 1) hit = all(c >= in);
            else             { float3 d = c - in; hit = dot(d, d) <= p[8]; }
            o = hit ? float3(p[5], p[6], p[7]) : c;
            break;
        }
        case 9: {
            float luma = dot(c, float3(0.299f, 0.587f, 0.114f));
            int idx = (int)(clamp(luma, 0.0f, 1.0f) * 255.0f);
            o = float3(lut[idx * 3], lut[idx * 3 + 1], lut[idx * 3 + 2]);
            break;
        }
    }
    dst.write(float4(wv_q8(o), 1.0), g);
}

// Effect List blend: dst = blend(dest, src) per AVS mode id (EffectList.h).
// p = [mode, adjustable]
kernel void fx_blend(texture2d<float, access::read> dsrc [[texture(0)]],
                     texture2d<float, access::read> ssrc [[texture(1)]],
                     texture2d<float, access::write> dst [[texture(2)]],
                     constant float* p [[buffer(0)]],
                     uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    float3 d = dsrc.read(g).rgb, s = ssrc.read(g).rgb, o = s;
    switch ((int)p[0]) {
        case 2:  o = (d + s) * 0.5f; break;
        case 3:  o = max(d, s); break;
        case 4:  o = min(d + s, 1.0f); break;
        case 5:  o = max(d - s, 0.0f); break;
        case 6:  o = max(s - d, 0.0f); break;
        case 7:  o = (g.y & 1u) == 0u ? s : d; break;
        case 8:  o = ((g.x + g.y) & 1u) == 0u ? s : d; break;
        case 9: {
            int3 a = int3(round(clamp(d, 0.0f, 1.0f) * 255.0f));
            int3 b = int3(round(clamp(s, 0.0f, 1.0f) * 255.0f));
            o = float3(a ^ b) / 255.0f;
            break;
        }
        case 10: { float a = clamp(p[1], 0.0f, 1.0f); o = s * a + d * (1.0f - a); break; }
        case 11: o = d * s; break;
        case 13: o = min(d, s); break;
        default: o = s; break;                    // 1 replace (and fallback)
    }
    dst.write(float4(wv_q8(o), 1.0), g);
}
)MSL";

// Picture pass (Picture.h). One oversized triangle covers the drawable; uv
// is derived from clip space so the framebuffer's bottom-up row order lands
// the right way up without a flip. The glow is a small pyramid computed from
// the frame into private textures; nothing here writes the frame itself.
static const char* kPictureShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut pic_vs(uint vid [[vertex_id]])
{
    float2 p = float2(vid == 2 ? 3.0 : -1.0, vid == 1 ? 3.0 : -1.0);
    VOut o; o.pos = float4(p, 0.0, 1.0); o.uv = p * 0.5 + 0.5; return o;
}

struct PicU {
    float2 srcSize, dstSize;
    float glow, lookStrength, lookSize, vignette;
    float scanlines, grain, bass, beat;
    float frame, pointScale, sharp, dither;
    float hasGlow, hasLook, beatReactive, glowLevels;
    float hdr, peak, linearOut, hasOverflow;
};

// Display-encoded (sRGB curve) to linear light, extended above 1 for EDR.
static float3 toLinearExt(float3 c)
{
    c = max(c, 0.0);
    return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045);
}

static uint hashu(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
static float rnd(uint2 p, uint seed)
{
    return float(hashu(p.x * 1973u + p.y * 9277u + seed * 26699u + 0x9e3779b9u)) * (1.0 / 4294967296.0);
}

// Bicubic Catmull-Rom from 9 bilinear taps, clamped to the 2x2 texels
// around the sample point so hard edges don't ring (no dark halo, no
// bright overshoot next to a thin line).
static float3 sampleSharp(texture2d<float> t, float2 uv, float2 size)
{
    constexpr sampler lin(filter::linear, address::clamp_to_edge);
    constexpr sampler nrst(filter::nearest, address::clamp_to_edge);
    float2 sp = uv * size;
    float2 t1 = floor(sp - 0.5) + 0.5;
    float2 f = sp - t1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 t12 = (t1 + w2 / w12) / size;
    float2 t0 = (t1 - 1.0) / size, t3 = (t1 + 2.0) / size;
    float3 r = 0.0;
    r += t.sample(lin, float2(t0.x,  t0.y)).rgb  * (w0.x  * w0.y);
    r += t.sample(lin, float2(t12.x, t0.y)).rgb  * (w12.x * w0.y);
    r += t.sample(lin, float2(t3.x,  t0.y)).rgb  * (w3.x  * w0.y);
    r += t.sample(lin, float2(t0.x,  t12.y)).rgb * (w0.x  * w12.y);
    r += t.sample(lin, float2(t12.x, t12.y)).rgb * (w12.x * w12.y);
    r += t.sample(lin, float2(t3.x,  t12.y)).rgb * (w3.x  * w12.y);
    r += t.sample(lin, float2(t0.x,  t3.y)).rgb  * (w0.x  * w3.y);
    r += t.sample(lin, float2(t12.x, t3.y)).rgb  * (w12.x * w3.y);
    r += t.sample(lin, float2(t3.x,  t3.y)).rgb  * (w3.x  * w3.y);
    float2 a = t1 / size, b = (t1 + 1.0) / size;
    float3 c0 = t.sample(nrst, a).rgb, c1 = t.sample(nrst, float2(b.x, a.y)).rgb;
    float3 c2 = t.sample(nrst, float2(a.x, b.y)).rgb, c3 = t.sample(nrst, b).rgb;
    return clamp(r, min(min(c0, c1), min(c2, c3)), max(max(c0, c1), max(c2, c3)));
}

// Order: scale → glow → (tone map, phase 3) → look → vignette → grain →
// scanlines → dither. The look comes after the glow so halos take the look
// too; vignette before grain because grain sits on the "film".
fragment float4 pic_fs(VOut in [[stage_in]],
                       texture2d<float> src [[texture(0)]],
                       texture2d<float> glowTex [[texture(1)]],
                       texture3d<float> lut [[texture(2)]],
                       texture2d<float> overflowTex [[texture(3)]],
                       constant PicU& u [[buffer(0)]])
{
    constexpr sampler lin(filter::linear, address::clamp_to_edge);
    float2 uv = in.uv;
    bool upscale = u.dstSize.x > u.srcSize.x + 0.5 || u.dstSize.y > u.srcSize.y + 0.5;
    float3 c = (u.sharp > 0.5 && upscale) ? sampleSharp(src, uv, u.srcSize) : src.sample(lin, uv).rgb;
    c = max(c, 0.0);

    if (u.hasGlow > 0.5) {
        float g = u.glow * (1.0 + u.beatReactive * 0.8 * u.bass);
        // The pyramid sums one copy of the thresholded light per level, each
        // spread wider; dividing by the depth gives about the source's own
        // energy, which a small bright feature spreads too thin to see. Full
        // glow adds several times that, so thin lines and points visibly bloom.
        c += glowTex.sample(lin, uv).rgb * (g * 7.0 / max(u.glowLevels, 1.0));
    }
    // HDR highlights: the light that clipped this frame (overflow, never
    // fed back). On an extended-range screen it becomes real brightness above
    // white in its own hue, rolled off toward the peak; everywhere it turns
    // the pixel white-hot. Pixels without overflow are exactly as in SDR.
    if (u.hasOverflow > 0.5) {
        float3 hot = max(overflowTex.sample(lin, uv).rgb, 0.0);
        float h = max(hot.r, max(hot.g, hot.b));
        if (h > 1e-4) {
            if (u.peak > 1.001) c += hot * ((u.peak - 1.0) * (1.0 - exp(-h / (u.peak - 1.0))) / h);
            float m = max(c.r, max(c.g, c.b));
            c = mix(c, float3(m), (1.0 - exp(-h * 1.2)) * 0.85);
        }
    }
    c = clamp(c, 0.0, u.linearOut > 0.5 ? u.peak : 1.0);

    if (u.hasLook > 0.5) {
        // The look is defined on 0..1; light above white (EDR) passes through.
        float n = u.lookSize;
        float3 base = min(c, 1.0), extra = c - base;
        float3 lc = lut.sample(lin, base * ((n - 1.0) / n) + 0.5 / n).rgb;
        c = mix(base, lc, u.lookStrength) + extra;
    }

    if (u.vignette > 0.0) {
        float aspect = u.dstSize.x / max(u.dstSize.y, 1.0);
        float2 d = (uv - 0.5) * float2(aspect, 1.0);
        float r = length(d) / length(float2(aspect, 1.0) * 0.5);   // 0 centre, 1 corner
        c *= 1.0 - u.vignette * 0.85 * smoothstep(0.3, 1.05, r);
    }

    uint fr = uint(u.frame);
    float ps = max(u.pointScale, 1.0);
    if (u.grain > 0.0) {
        uint2 cell = uint2(in.pos.xy / ps);                 // one grain per point
        float n = rnd(cell, fr * 2u + 11u) + rnd(cell, fr * 2u + 12u) - 1.0;
        float y = min(dot(c, float3(0.2126, 0.7152, 0.0722)), 1.0);
        float w = 0.2 + 3.2 * y * (1.0 - y);                 // strongest in the midtones
        float g = u.grain * (1.0 + u.beatReactive * 0.6 * u.beat);
        c += n * g * 0.11 * w;
    }

    if (u.scanlines > 0.0) {
        float s = 0.5 + 0.5 * cos(in.pos.y / ps * (2.0 * M_PI_F / 3.0));   // 3-point pitch
        c *= 1.0 - u.scanlines * 0.5 * (1.0 - s);
    }

    if (u.linearOut > 0.5)                                   // extended range: a float layer, no dither needed
        return float4(toLinearExt(min(c, u.peak)), 1.0);
    if (u.dither > 0.5) {
        uint2 px = uint2(in.pos.xy);
        float n = rnd(px, fr * 3u + 1u) + rnd(px, fr * 3u + 2u) - 1.0;   // triangular, ±1 LSB
        c += n * (1.0 / 255.0);
    }
    return float4(clamp(c, 0.0, 1.0), 1.0);
}

// ---- glow pyramid: prefilter (soft threshold) → downsample chain → tent
// upsample-and-add back up. Result: a wide, smooth, energy-sane halo.
struct GlowU { float2 srcTexel; float threshold; float knee; };

static float3 box5(texture2d<float, access::sample> src, float2 uv, float2 o)
{
    constexpr sampler lin(filter::linear, address::clamp_to_edge);
    return src.sample(lin, uv).rgb * 0.5
         + (src.sample(lin, uv + float2(-o.x, -o.y)).rgb + src.sample(lin, uv + float2(o.x, -o.y)).rgb
          + src.sample(lin, uv + float2(-o.x,  o.y)).rgb + src.sample(lin, uv + float2(o.x,  o.y)).rgb) * 0.125;
}

kernel void pic_prefilter(texture2d<float, access::sample> src [[texture(0)]],
                          texture2d<float, access::write> dst [[texture(1)]],
                          constant GlowU& u [[buffer(0)]],
                          uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    float2 uv = (float2(g) + 0.5) / float2(dst.get_width(), dst.get_height());
    float3 c = clamp(box5(src, uv, u.srcTexel), 0.0, 4.0);
    if (u.knee < 0.0) {
        // The Bloom effect's threshold (as it always was): on luminance,
        // rescaled so light at the threshold adds nothing. Saturated blues
        // and reds, dim in luminance, barely glow; presets that feed their
        // frame back (Neon Cathedral) are tuned on that.
        float lum = dot(c, float3(0.299, 0.587, 0.114));
        float k = lum <= u.threshold ? 0.0 : (lum - u.threshold) / max(1.0 - u.threshold, 1e-4);
        dst.write(float4(c * k, 1.0), g);
        return;
    }
    float br = max(c.r, max(c.g, c.b));
    float soft = clamp(br - u.threshold + u.knee, 0.0, 2.0 * u.knee);
    soft = soft * soft / (4.0 * u.knee + 1e-4);
    float w = max(soft, br - u.threshold) / max(br, 1e-4);
    dst.write(float4(c * w, 1.0), g);
}

kernel void pic_down(texture2d<float, access::sample> src [[texture(0)]],
                     texture2d<float, access::write> dst [[texture(1)]],
                     constant GlowU& u [[buffer(0)]],
                     uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    float2 uv = (float2(g) + 0.5) / float2(dst.get_width(), dst.get_height());
    dst.write(float4(box5(src, uv, u.srcTexel), 1.0), g);
}

// The in-stack Bloom effect's combine: frame + pyramid glow with the
// effect's soft rolloff (1 - e^-x), as always. With HDR, the light above
// white it rolled off is also added to the overflow buffer (ovIn → ovOut).
struct BloomU { float gain; float hdr; float pad0; float pad1; };
kernel void pic_bloom_add(texture2d<float, access::read> base [[texture(0)]],
                          texture2d<float, access::sample> glow [[texture(1)]],
                          texture2d<float, access::write> dst [[texture(2)]],
                          texture2d<float, access::read> ovIn [[texture(3)]],
                          texture2d<float, access::write> ovOut [[texture(4)]],
                          constant BloomU& u [[buffer(0)]],
                          uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler lin(filter::linear, address::clamp_to_edge);
    float2 uv = (float2(g) + 0.5) / float2(dst.get_width(), dst.get_height());
    float3 x = max(base.read(g).rgb + glow.sample(lin, uv).rgb * u.gain, 0.0);
    dst.write(float4(1.0 - exp(-x), 1.0), g);
    if (u.hdr > 0.5) ovOut.write(float4(ovIn.read(g).rgb + max(x - 1.0, 0.0), 1.0), g);
}

// The in-stack Tone Map, HDR: the same SDR result, plus the exposed light
// above white into the overflow buffer.
kernel void pic_tonemap_hdr(texture2d<float, access::read> src [[texture(0)]],
                            texture2d<float, access::write> dst [[texture(1)]],
                            texture2d<float, access::read> ovIn [[texture(2)]],
                            texture2d<float, access::write> ovOut [[texture(3)]],
                            constant float4& p [[buffer(0)]],
                            uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    float3 x = max(src.read(g).rgb * p.x, 0.0);
    float3 c = 1.0 - exp(-x);
    c = pow(c, float3(p.y));
    float lum = dot(c, float3(0.299, 0.587, 0.114));
    c = mix(float3(lum), c, p.z);
    dst.write(float4(clamp(c, 0.0, 1.0), 1.0), g);
    ovOut.write(float4(ovIn.read(g).rgb + max(x - 1.0, 0.0), 1.0), g);
}

kernel void pic_up(texture2d<float, access::sample> coarse [[texture(0)]],
                   texture2d<float, access::read> fine [[texture(1)]],
                   texture2d<float, access::write> dst [[texture(2)]],
                   constant GlowU& u [[buffer(0)]],
                   uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler lin(filter::linear, address::clamp_to_edge);
    float2 uv = (float2(g) + 0.5) / float2(dst.get_width(), dst.get_height());
    float2 o = u.srcTexel;
    float3 s = coarse.sample(lin, uv).rgb * 4.0
             + (coarse.sample(lin, uv + float2(o.x, 0)).rgb + coarse.sample(lin, uv - float2(o.x, 0)).rgb
              + coarse.sample(lin, uv + float2(0, o.y)).rgb + coarse.sample(lin, uv - float2(0, o.y)).rgb) * 2.0
             + (coarse.sample(lin, uv + o).rgb + coarse.sample(lin, uv - o).rgb
              + coarse.sample(lin, uv + float2(o.x, -o.y)).rgb + coarse.sample(lin, uv + float2(-o.x, o.y)).rgb);
    dst.write(float4(fine.read(g).rgb + s * (1.0 / 16.0), 1.0), g);
}
)MSL";

// A framebuffer's pixels wrapped as a Metal resource: the MTLBuffer aliases
// the vector's mmap'd storage (no copy), the texture is a linear view of it.
struct Wrap {
    id<MTLBuffer> buf = nil;
    id<MTLTexture> tex = nil;
    int w = 0, h = 0;
    size_t bytes = 0;
};

// (Non-ARC caveat, see Pass below: initialize every ObjC pointer.)
struct Ctx {
    id<MTLDevice> dev = nil;
    id<MTLCommandQueue> queue = nil;
    NSDictionary<NSString*, id<MTLComputePipelineState>>* pipes = nil;
    // Picture pass (lazily built by present()/renderPicture()).
    id<MTLLibrary> picLib = nil;
    // One render pipeline per output format: the SDR layer (bgra8), the
    // extended-range layer (rgba16Float), float readback for tests.
    static constexpr int kPicFormats = 4;
    id<MTLRenderPipelineState> picPipe[kPicFormats] = {};
    MTLPixelFormat picPipeFormat[kPicFormats] = {};
    id<MTLComputePipelineState> picPrefilter = nil, picDown = nil, picUp = nil, picBloomAdd = nil,
                                picToneHdr = nil;
    static constexpr int kGlowMax = 6;
    id<MTLTexture> glowDown[kGlowMax] = {};
    id<MTLTexture> glowUp[kGlowMax] = {};
    int glowLevels = 0, glowW = 0, glowH = 0;
    id<MTLTexture> lutTex = nil;
    uint64_t lutVersion = 0;
    int lutSize = 0;
    id<MTLTexture> dummy2D = nil, dummy3D = nil;   // bound when glow/look are off
    bool broken = false;                            // a wrap failed: fall back to CPU paths

    // Deferred execution: ops encode into one pending command buffer and
    // nobody waits until the CPU actually needs pixels (flush()). Encoders
    // in one command buffer execute in order with Metal tracking the
    // resource hazards, so consecutive GPU effects cost one round trip
    // instead of one each.
    id<MTLCommandBuffer> pending = nil;
    int pendingOps = 0;

    // Small per-op data (grids, tables, LUTs): a ring so a pending op keeps
    // its own copy; the ring wrapping forces a flush.
    static constexpr int kSmall = 16;
    id<MTLBuffer> small[kSmall] = {};
    int smallIdx = 0;

    // Private scratch textures for multi-pass ops, recreated on size change.
    int w = 0, h = 0;
    id<MTLTexture> tex[2] = {nil, nil};

    // Wrappers keyed by the framebuffer's pixel pointer; dropped by the
    // Framebuffer allocator hook before the memory is unmapped.
    std::unordered_map<const void*, Wrap> wraps;

    void flush() {
        if (!pending) return;
        [pending commit];
        [pending waitUntilCompleted];
#if !__has_feature(objc_arc)
        [pending release];
#endif
        pending = nil;
        pendingOps = 0;
    }

    id<MTLCommandBuffer> cb() {
        if (!pending) {
            pending = [queue commandBuffer];
#if !__has_feature(objc_arc)
            [pending retain];
#endif
        }
        return pending;
    }

    bool ensureSize(int W, int H) {
        if (w == W && h == H && tex[0]) return true;
        flush();                                   // pending ops may use the old scratch
        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                         width:(NSUInteger)W height:(NSUInteger)H
                                     mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModePrivate;
        for (auto& t : tex) {
            t = [dev newTextureWithDescriptor:td];
            if (!t) return false;
        }
        w = W; h = H;
        return true;
    }
};

static Ctx* gCtx = nullptr;                          // set once ctx() succeeds

static void releaseWrap(Wrap& wv)
{
#if !__has_feature(objc_arc)
    [wv.tex release];
    [wv.buf release];
#endif
    wv.tex = nil;
    wv.buf = nil;
}

// Framebuffer allocator hook: the memory behind `p` is about to be unmapped.
static void onFramebufferMemoryFreed(const void* p)
{
    if (!gCtx) return;
    auto it = gCtx->wraps.find(p);
    if (it != gCtx->wraps.end()) {
        gCtx->flush();                             // a pending op may still read/write it
        releaseWrap(it->second);
        gCtx->wraps.erase(it);
    }
}

static Ctx* ctx()
{
    static Ctx* c = [] () -> Ctx* {
        if (getenv("MSCOPES_NO_GPU")) return nullptr;      // forced CPU paths
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) return nullptr;
        NSError* err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(kKernels) options:nil error:&err];
        if (!lib) {
            NSLog(@"[gpu] kernel compile failed: %@", err);
            return nullptr;
        }
        id<MTLLibrary> lib2 = [dev newLibraryWithSource:@(kClassicKernels) options:nil error:&err];
        if (!lib2) {
            NSLog(@"[gpu] classic-port kernel compile failed: %@", err);
            return nullptr;
        }
        NSMutableDictionary* pipes = [NSMutableDictionary dictionary];
        auto build = [&](id<MTLLibrary> l, NSArray<NSString*>* names) -> bool {
            for (NSString* name in names) {
                id<MTLFunction> fn = [l newFunctionWithName:name];
                id<MTLComputePipelineState> ps = fn ? [dev newComputePipelineStateWithFunction:fn error:&err] : nil;
                if (!ps) { NSLog(@"[gpu] pipeline %@ failed: %@", name, err); return false; }
                pipes[name] = ps;
            }
            return true;
        };
        if (!build(lib, @[@"fx_threshold", @"fx_blur", @"fx_bloom_combine",
                          @"fx_kaleido", @"fx_rgbsplit",
                          @"fx_tonemap", @"fx_vignette", @"fx_trails",
                          @"fx_shockwave", @"fx_glitch",
                          @"fx_streak_combine", @"fx_crt", @"fx_shimmer",
                          @"fx_lens", @"fx_radialblur", @"fx_edges", @"fx_duotone"]))
            return nullptr;
        if (!build(lib2, @[@"fx_warp", @"fx_boxblur", @"fx_gridwarp", @"fx_conv5",
                           @"fx_shift", @"fx_distmod", @"fx_colorop", @"fx_blend"]))
            return nullptr;
        Ctx* c = new Ctx;
        c->dev = dev;
        c->queue = [dev newCommandQueue];
        c->pipes = pipes;
        gCtx = c;
        gFramebufferMemoryFreed = onFramebufferMemoryFreed;
        return c;
    }();
    return c;
}

bool available()
{
    Ctx* c = ctx();
    return c != nullptr && !c->broken;
}

void* metalDevice()
{
    Ctx* c = ctx();
    return c ? (__bridge void*)c->dev : nullptr;
}

// ---------------------------------------------------------------------------
//  Pass plumbing
// ---------------------------------------------------------------------------
// The texture aliasing fb's pixels (created on first use, cached by pointer).
static id<MTLTexture> fbTexture(Ctx* c, const Framebuffer& fb)
{
    const void* p = fb.px.data();
    if (!p || fb.w <= 0 || fb.h <= 0 || fb.px.size() < (size_t)fb.w * fb.h * 4) return nil;
    const size_t bytes = PageAllocator<float>::bytesFor((size_t)fb.w * fb.h * 4);
    auto it = c->wraps.find(p);
    if (it != c->wraps.end()) {
        Wrap& wv = it->second;
        if (wv.w == fb.w && wv.h == fb.h && wv.bytes == bytes) return wv.tex;
        releaseWrap(wv);                       // same memory, new geometry: rewrap
        c->wraps.erase(it);
    }
    Wrap wv;
    wv.buf = [c->dev newBufferWithBytesNoCopy:const_cast<void*>(p) length:bytes
                                      options:MTLResourceStorageModeShared deallocator:nil];
    if (wv.buf) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                         width:(NSUInteger)fb.w height:(NSUInteger)fb.h
                                     mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModeShared;
        wv.tex = [wv.buf newTextureWithDescriptor:td offset:0
                                      bytesPerRow:(NSUInteger)fb.w * 4 * sizeof(float)];
    }
    if (!wv.tex) {
        NSLog(@"[gpu] cannot wrap framebuffer memory as a texture; using CPU paths");
        releaseWrap(wv);
        c->broken = true;
        return nil;
    }
    wv.w = fb.w; wv.h = fb.h; wv.bytes = bytes;
    c->wraps[p] = wv;
    return wv.tex;
}

// Common op prologue: context, scratch pool sized, fb's texture.
static id<MTLTexture> beginOp(Ctx*& c, const Framebuffer& fb)
{
    c = ctx();
    if (!c || c->broken || fb.w <= 0 || fb.h <= 0 || !c->ensureSize(fb.w, fb.h)) return nil;
    return fbTexture(c, fb);
}

// The CPU is about to touch pixels: finish whatever the GPU still owes.
void flush()
{
    if (gCtx) gCtx->flush();
}
void invalidateResident(const void*) { flush(); }
void syncToCpu(Framebuffer&) { flush(); }
void syncToCpuForRead(const Framebuffer&) { flush(); }

// NOTE: every ObjC pointer here carries an explicit initializer. This file
// is built without ARC in the CLI tools (plain clang++), where an
// uninitialized `id` member is stack garbage, not nil — a Pass filled
// field-by-field once crashed in setTexture: on exactly that.
struct Pass {
    NSString* kernel = nil;
    id<MTLTexture> in0 = nil, in1 = nil, out = nil;
    float params[40] = {};              // room for a 5x5 kernel + options
    int nParams = 0;
    id<MTLBuffer> buf1 = nil;           // optional extra input at buffer(1)
};

// Encode the passes into the pending command buffer and copy the last
// output back into the framebuffer's own texture (i.e. its memory). Nothing
// executes until flush() — the next CPU consumer — so consecutive GPU
// effects share one round trip. Kernels gather from their source, so they
// never write in place; the final on-GPU copy is the price (~0.1 ms for
// 15 MB), versus the two CPU-side twiddled copies the old resident-texture
// design paid per boundary.
static void run(Ctx* c, Pass* passes, int nPasses, int W, int H,
                id<MTLTexture> result, id<MTLTexture> fbTex)
{
    id<MTLCommandBuffer> cb = c->cb();
    for (int i = 0; i < nPasses; ++i) {
        Pass& p = passes[i];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:c->pipes[p.kernel]];
        int ti = 0;
        [e setTexture:p.in0 atIndex:ti++];
        if (p.in1) [e setTexture:p.in1 atIndex:ti++];
        [e setTexture:p.out atIndex:ti];
        [e setBytes:p.params length:sizeof(p.params) atIndex:0];
        if (p.buf1) [e setBuffer:p.buf1 offset:0 atIndex:1];
        MTLSize tg = MTLSizeMake(16, 16, 1);
        MTLSize grid = MTLSizeMake(((NSUInteger)W + 15) / 16, ((NSUInteger)H + 15) / 16, 1);
        [e dispatchThreadgroups:grid threadsPerThreadgroup:tg];
        [e endEncoding];
    }
    if (result != fbTex) {
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:result sourceSlice:0 sourceLevel:0
              sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake((NSUInteger)W, (NSUInteger)H, 1)
                 toTexture:fbTex destinationSlice:0 destinationLevel:0
         destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b endEncoding];
    }
    if (++c->pendingOps >= 64) c->flush();     // bound a never-consumed queue
}

// ---------------------------------------------------------------------------
//  Presentation
// ---------------------------------------------------------------------------
static double gPresentUploadMs = 0, gPresentDrawableMs = 0, gPresentGpuMs = 0;
void lastPresentTimes(double* u, double* d, double* g)
{
    if (u) *u = gPresentUploadMs;
    if (d) *d = gPresentDrawableMs;
    if (g) *g = gPresentGpuMs;
}

#if __has_feature(objc_arc)
#define WV_RELEASE(x) ((x) = nil)
#else
#define WV_RELEASE(x) do { [(x) release]; (x) = nil; } while (0)
#endif

// Mirror of the shader's PicU / GlowU (all 4-byte floats; the float2 pairs
// come first so offsets match Metal's 8-byte alignment).
struct PicU {
    float srcW, srcH, dstW, dstH;
    float glow, lookStrength, lookSize, vignette;
    float scanlines, grain, bass, beat;
    float frame, pointScale, sharp, dither;
    float hasGlow, hasLook, beatReactive, glowLevels;
    float hdr, peak, linearOut, hasOverflow;
};
struct GlowU { float tx, ty, threshold, knee; };
struct BloomU { float gain, hdr, pad0, pad1; };

// The picture library, its compute pipelines (glow pyramid, bloom) and the
// dummy textures; shared by the picture pass and the in-stack Bloom.
static bool ensurePictureCompute(Ctx* c)
{
    if (c->picLib) return c->picPrefilter && c->picDown && c->picUp && c->picBloomAdd && c->picToneHdr;
    NSError* err = nil;
    c->picLib = [c->dev newLibraryWithSource:@(kPictureShaders) options:nil error:&err];
    if (!c->picLib) { NSLog(@"[gpu] picture shaders failed: %@", err); return false; }
    auto comp = [&](NSString* name) -> id<MTLComputePipelineState> {
        id<MTLFunction> fn = [c->picLib newFunctionWithName:name];
        id<MTLComputePipelineState> ps = fn ? [c->dev newComputePipelineStateWithFunction:fn error:&err] : nil;
#if !__has_feature(objc_arc)
        [fn release];
#endif
        if (!ps) NSLog(@"[gpu] picture pipeline %@ failed: %@", name, err);
        return ps;
    };
    c->picPrefilter = comp(@"pic_prefilter");
    c->picDown = comp(@"pic_down");
    c->picUp = comp(@"pic_up");
    c->picBloomAdd = comp(@"pic_bloom_add");
    c->picToneHdr = comp(@"pic_tonemap_hdr");
    if (!c->picPrefilter || !c->picDown || !c->picUp || !c->picBloomAdd || !c->picToneHdr) return false;

    MTLTextureDescriptor* d2 = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:1 height:1 mipmapped:NO];
    d2.usage = MTLTextureUsageShaderRead;
    c->dummy2D = [c->dev newTextureWithDescriptor:d2];
    MTLTextureDescriptor* d3 = [MTLTextureDescriptor new];
    d3.textureType = MTLTextureType3D;
    d3.pixelFormat = MTLPixelFormatRGBA16Float;
    d3.width = d3.height = d3.depth = 1;
    d3.usage = MTLTextureUsageShaderRead;
    c->dummy3D = [c->dev newTextureWithDescriptor:d3];
#if !__has_feature(objc_arc)
    [d3 release];
#endif
    return c->dummy2D && c->dummy3D;
}

// The picture pass's render pipeline for an output format.
static id<MTLRenderPipelineState> picturePipeline(Ctx* c, MTLPixelFormat fmt)
{
    if (!ensurePictureCompute(c)) return nil;
    int freeSlot = -1;
    for (int i = 0; i < Ctx::kPicFormats; ++i) {
        if (c->picPipe[i] && c->picPipeFormat[i] == fmt) return c->picPipe[i];
        if (!c->picPipe[i] && freeSlot < 0) freeSlot = i;
    }
    if (freeSlot < 0) { freeSlot = 0; WV_RELEASE(c->picPipe[0]); }
    NSError* err = nil;
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    id<MTLFunction> vs = [c->picLib newFunctionWithName:@"pic_vs"];
    id<MTLFunction> fs = [c->picLib newFunctionWithName:@"pic_fs"];
    d.vertexFunction = vs;
    d.fragmentFunction = fs;
    d.colorAttachments[0].pixelFormat = fmt;
    c->picPipe[freeSlot] = [c->dev newRenderPipelineStateWithDescriptor:d error:&err];
#if !__has_feature(objc_arc)
    [vs release]; [fs release]; [d release];
#endif
    if (!c->picPipe[freeSlot]) { NSLog(@"[gpu] picture render pipeline failed: %@", err); return nil; }
    c->picPipeFormat[freeSlot] = fmt;
    return c->picPipe[freeSlot];
}

// The glow pyramid for a W x H frame: level 0 at half size, halving until
// the short side would drop under 8 pixels (at most kGlowMax levels).
static bool ensureGlow(Ctx* c, int W, int H)
{
    if (c->glowW == W && c->glowH == H && c->glowLevels > 0) return true;
    for (int i = 0; i < Ctx::kGlowMax; ++i) {
        if (c->glowDown[i]) WV_RELEASE(c->glowDown[i]);
        if (c->glowUp[i]) WV_RELEASE(c->glowUp[i]);
    }
    c->glowLevels = 0;
    int w = std::max(1, W / 2), h = std::max(1, H / 2);
    for (int i = 0; i < Ctx::kGlowMax && std::min(w, h) >= 8; ++i) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                         width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModePrivate;
        c->glowDown[i] = [c->dev newTextureWithDescriptor:td];
        c->glowUp[i] = [c->dev newTextureWithDescriptor:td];
        if (!c->glowDown[i] || !c->glowUp[i]) return false;
        c->glowLevels = i + 1;
        w = std::max(1, w / 2); h = std::max(1, h / 2);
    }
    c->glowW = W; c->glowH = H;
    return c->glowLevels > 0;
}

static bool uploadLook(Ctx* c, const LookTable& t)
{
    if (c->lutTex && c->lutVersion == t.version && c->lutSize == t.size) return true;
    const int n = t.size;
    if (!c->lutTex || c->lutSize != n) {
        if (c->lutTex) WV_RELEASE(c->lutTex);
        MTLTextureDescriptor* d = [MTLTextureDescriptor new];
        d.textureType = MTLTextureType3D;
        d.pixelFormat = MTLPixelFormatRGBA32Float;
        d.width = d.height = d.depth = (NSUInteger)n;
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = c->dev.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
        c->lutTex = [c->dev newTextureWithDescriptor:d];
#if !__has_feature(objc_arc)
        [d release];
#endif
        if (!c->lutTex) return false;
        c->lutSize = n;
    }
    std::vector<float> rgba((size_t)n * n * n * 4);
    for (size_t i = 0, j = 0; i < (size_t)n * n * n; ++i) {
        rgba[j++] = t.rgb[i * 3]; rgba[j++] = t.rgb[i * 3 + 1]; rgba[j++] = t.rgb[i * 3 + 2]; rgba[j++] = 1.f;
    }
    // Every earlier user of the table has completed: present() and
    // renderPicture() wait for their command buffer before returning.
    [c->lutTex replaceRegion:MTLRegionMake3D(0, 0, 0, (NSUInteger)n, (NSUInteger)n, (NSUInteger)n)
                 mipmapLevel:0 slice:0 withBytes:rgba.data()
                 bytesPerRow:(NSUInteger)n * 4 * sizeof(float)
               bytesPerImage:(NSUInteger)n * n * 4 * sizeof(float)];
    c->lutVersion = t.version;
    return true;
}

static void dispatch2D(id<MTLCommandBuffer> cb, id<MTLComputePipelineState> ps,
                       id<MTLTexture> t0, id<MTLTexture> t1, id<MTLTexture> t2, const GlowU& u)
{
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:ps];
    [e setTexture:t0 atIndex:0];
    [e setTexture:t1 atIndex:1];
    if (t2) [e setTexture:t2 atIndex:2];
    [e setBytes:&u length:sizeof(u) atIndex:0];
    id<MTLTexture> out = t2 ? t2 : t1;
    MTLSize tg = MTLSizeMake(16, 16, 1);
    MTLSize grid = MTLSizeMake((out.width + 15) / 16, (out.height + 15) / 16, 1);
    [e dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [e endEncoding];
}

// Encode the whole picture pass: the glow pyramid (if any) and the one
// fragment pass into `rp`'s colour attachment.
static void encodePicture(Ctx* c, id<MTLCommandBuffer> cb, id<MTLTexture> src, int sw, int sh,
                          MTLRenderPassDescriptor* rp, int dw, int dh, const PictureSettings& s,
                          id<MTLRenderPipelineState> pipe)
{
    const bool glow = s.glow > 0.001f && ensureGlow(c, sw, sh);
    if (glow) {
        GlowU u = { 1.f / sw, 1.f / sh, 0.5f, 0.4f };       // soft threshold: highlights, not mids
        dispatch2D(cb, c->picPrefilter, src, c->glowDown[0], nil, u);
        for (int i = 1; i < c->glowLevels; ++i) {
            id<MTLTexture> prev = c->glowDown[i - 1];
            GlowU d = { 1.f / prev.width, 1.f / prev.height, 0, 0 };
            dispatch2D(cb, c->picDown, prev, c->glowDown[i], nil, d);
        }
        int L = c->glowLevels;
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:c->glowDown[L - 1] toTexture:c->glowUp[L - 1]];
        [bl endEncoding];
        for (int i = L - 2; i >= 0; --i) {
            id<MTLTexture> coarse = c->glowUp[i + 1];
            GlowU d = { 1.f / coarse.width, 1.f / coarse.height, 0, 0 };
            dispatch2D(cb, c->picUp, coarse, c->glowDown[i], c->glowUp[i], d);
        }
    }
    const bool look = s.hasLook() && uploadLook(c, *s.look);
    id<MTLTexture> ovTex = nil;
    if (s.hdr && s.overflow && s.overflow->w == sw && s.overflow->h == sh)
        ovTex = fbTexture(c, *s.overflow);

    PicU u = {};
    u.srcW = (float)sw; u.srcH = (float)sh; u.dstW = (float)dw; u.dstH = (float)dh;
    u.glow = s.glow; u.lookStrength = std::clamp(s.lookStrength, 0.f, 1.f);
    u.lookSize = look ? (float)c->lutSize : 1.f;
    u.vignette = std::clamp(s.vignette, 0.f, 1.f);
    u.scanlines = std::clamp(s.scanlines, 0.f, 1.f);
    u.grain = std::clamp(s.grain, 0.f, 1.f);
    u.bass = std::clamp(s.bass, 0.f, 1.f);
    u.beat = std::clamp(s.beat, 0.f, 1.f);
    u.frame = (float)(s.frame & 0xffff);
    u.pointScale = std::max(1.f, s.pointScale);
    u.sharp = s.sharpScaling ? 1.f : 0.f;
    u.dither = s.dither ? 1.f : 0.f;
    u.hasGlow = glow ? 1.f : 0.f;
    u.hasLook = look ? 1.f : 0.f;
    u.beatReactive = s.beatReactive ? 1.f : 0.f;
    u.glowLevels = (float)(glow ? c->glowLevels : 1);
    // HDR: the peak in display-encoded units (sRGB curve of the linear
    // headroom), capped at 4x white so highlights glow without glaring.
    const float headroom = s.linearOutput ? std::clamp(s.headroom, 1.f, 4.f) : 1.f;
    u.hdr = s.hdr ? 1.f : 0.f;
    u.peak = headroom <= 1.f ? 1.f : 1.055f * std::pow(headroom, 1.f / 2.4f) - 0.055f;
    u.linearOut = s.linearOutput ? 1.f : 0.f;
    u.hasOverflow = ovTex ? 1.f : 0.f;

    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:pipe];
    [e setFragmentTexture:src atIndex:0];
    [e setFragmentTexture:(glow ? c->glowUp[0] : c->dummy2D) atIndex:1];
    [e setFragmentTexture:(look ? c->lutTex : c->dummy3D) atIndex:2];
    [e setFragmentTexture:(ovTex ? ovTex : c->dummy2D) atIndex:3];
    [e setFragmentBytes:&u length:sizeof(u) atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

bool present(const Framebuffer& fb, void* layerPtr, const PictureSettings& picture)
{
    Ctx* c = ctx();
    if (!c || !layerPtr || fb.w == 0 || fb.h == 0) return false;
    CAMetalLayer* layer = (__bridge CAMetalLayer*)layerPtr;
    auto nowMs = [] { return (double)clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) * 1e-6; };

    @autoreleasepool {
        id<MTLRenderPipelineState> pipe = picturePipeline(c, layer.pixelFormat);
        if (!pipe) return false;

        double t0 = nowMs();
        id<MTLTexture> R = fbTexture(c, fb);       // zero-copy view of the frame
        if (!R) return false;
        double t1 = nowMs();
        gPresentUploadMs = t1 - t0;

        id<CAMetalDrawable> drawable = [layer nextDrawable];
        double t2 = nowMs();
        gPresentDrawableMs = t2 - t1;
        if (!drawable) return true;                 // drawable pool exhausted: drop this frame

        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;

        // The pass joins whatever the frame's effects left pending, so a
        // GPU-only stack is a single command buffer from first kernel to
        // screen. The frame's memory is the CPU's and gets rewritten next
        // tick, so this is where the frame's work is finally waited for.
        id<MTLCommandBuffer> cb = c->cb();
        encodePicture(c, cb, R, fb.w, fb.h, rp,
                      (int)drawable.texture.width, (int)drawable.texture.height, picture, pipe);
        [cb presentDrawable:drawable];
        double t3 = nowMs();
        c->flush();
        gPresentGpuMs = nowMs() - t3;
    }
    return true;
}

// Offscreen picture pass into `fmt`, read back row-flipped so row 0 is the
// framebuffer's row 0. BGRA8 → rgba8 bytes; RGBA32Float → floats.
static bool renderPictureTo(const Framebuffer& fb, const PictureSettings& picture, int outW, int outH,
                            MTLPixelFormat fmt, std::vector<uint8_t>* bytes, std::vector<float>* floats)
{
    Ctx* c = ctx();
    if (!c || c->broken || fb.w == 0 || fb.h == 0 || outW <= 0 || outH <= 0) return false;
    bool ok = false;
    @autoreleasepool {
        id<MTLRenderPipelineState> pipe = picturePipeline(c, fmt);
        if (!pipe) return false;
        id<MTLTexture> R = fbTexture(c, fb);
        if (!R) return false;
        const bool isFloat = fmt == MTLPixelFormatRGBA32Float;
        const NSUInteger px = isFloat ? 16 : 4;
        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:fmt width:(NSUInteger)outW height:(NSUInteger)outH mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> target = [c->dev newTextureWithDescriptor:td];
        const NSUInteger rowBytes = (NSUInteger)outW * px;
        id<MTLBuffer> readback = [c->dev newBufferWithLength:rowBytes * (NSUInteger)outH
                                                     options:MTLResourceStorageModeShared];
        if (target && readback) {
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = target;
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = c->cb();
            encodePicture(c, cb, R, fb.w, fb.h, rp, outW, outH, picture, pipe);
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromTexture:target sourceSlice:0 sourceLevel:0
                  sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake((NSUInteger)outW, (NSUInteger)outH, 1)
                      toBuffer:readback destinationOffset:0
             destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes * (NSUInteger)outH];
            [b endEncoding];
            c->flush();
            // The render target's row 0 is the top of the image, where the
            // framebuffer keeps its last row: flip so row 0 matches fb row 0.
            const uint8_t* p = (const uint8_t*)readback.contents;
            if (isFloat && floats) {
                floats->resize((size_t)outW * outH * 4);
                for (int y = 0; y < outH; ++y)
                    std::memcpy(floats->data() + (size_t)y * outW * 4,
                                p + (size_t)(outH - 1 - y) * rowBytes, rowBytes);
                ok = true;
            } else if (!isFloat && bytes) {
                bytes->resize((size_t)outW * outH * 4);
                for (int y = 0; y < outH; ++y) {
                    const uint8_t* srow = p + (size_t)(outH - 1 - y) * rowBytes;
                    uint8_t* drow = bytes->data() + (size_t)y * rowBytes;
                    for (int x = 0; x < outW; ++x) {
                        drow[x * 4 + 0] = srow[x * 4 + 2];
                        drow[x * 4 + 1] = srow[x * 4 + 1];
                        drow[x * 4 + 2] = srow[x * 4 + 0];
                        drow[x * 4 + 3] = 255;
                    }
                }
                ok = true;
            }
        }
        if (target) WV_RELEASE(target);
        if (readback) WV_RELEASE(readback);
    }
    return ok;
}

bool renderPicture(const Framebuffer& fb, const PictureSettings& picture,
                   int outW, int outH, std::vector<uint8_t>& rgba)
{
    PictureSettings s = picture;
    s.linearOutput = false;                     // an 8-bit image: the SDR path
    return renderPictureTo(fb, s, outW, outH, MTLPixelFormatBGRA8Unorm, &rgba, nullptr);
}

bool renderPictureLinear(const Framebuffer& fb, const PictureSettings& picture,
                         int outW, int outH, std::vector<float>& rgba)
{
    PictureSettings s = picture;
    s.linearOutput = true;                      // what an extended-range layer receives
    return renderPictureTo(fb, s, outW, outH, MTLPixelFormatRGBA32Float, nullptr, &rgba);
}

// ---------------------------------------------------------------------------
//  Classic ports
// ---------------------------------------------------------------------------
// Every op: R = the framebuffer's own (zero-copy) texture, A/B = private
// scratch; the last pass's output is copied back into R by run().
static void onePass(Framebuffer& fb, NSString* kernel, std::initializer_list<float> params);

void movement(Framebuffer& fb, int mode, bool wrap, bool blend, int seed)
{
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    @autoreleasepool {
        id<MTLTexture> A = c->tex[0];
        Pass p[1];
        p[0] = {@"fx_warp", R, nil, A,
                {(float)mode, wrap ? 1.f : 0.f, blend ? 1.f : 0.f, (float)(seed & 0xffff)}, 4};
        run(c, p, 1, fb.w, fb.h, A, R);
    }
}

void rotoBlit(Framebuffer& fb, float ca, float sa, bool blend)
{
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    @autoreleasepool {
        id<MTLTexture> A = c->tex[0];
        Pass p[1];
        p[0] = {@"fx_warp", R, nil, A, {9.f, 1.f, blend ? 1.f : 0.f, 0.f, ca, sa}, 6};
        run(c, p, 1, fb.w, fb.h, A, R);
    }
}

void feedbackWarp(Framebuffer& fb, const Framebuffer& prev, float ca, float sa, float decay)
{
    if (prev.w != fb.w || prev.h != fb.h) return;
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    id<MTLTexture> P = fbTexture(c, prev);          // prev is read in place too
    if (!P) return;
    @autoreleasepool {
        Pass p[1];
        p[0] = {@"fx_warp", P, nil, R, {10.f, 0.f, 0.f, 0.f, ca, sa, decay}, 7};
        run(c, p, 1, fb.w, fb.h, R, R);             // writes fb directly (reads only prev)
    }
}

void boxBlur(Framebuffer& fb, int radius, float mix)
{
    if (radius <= 0) return;
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    @autoreleasepool {
        id<MTLTexture> A = c->tex[0], B = c->tex[1];
        float r = (float)std::min(radius, 64);
        Pass p[2];
        p[0] = {@"fx_boxblur", R, R, A, {r, 1, 0, 1.f}, 4};
        p[1] = {@"fx_boxblur", A, R, B, {r, 0, 1, mix}, 4};
        run(c, p, 2, fb.w, fb.h, B, R);
    }
}

// A small-data buffer (grid, table, LUT) for one op, from the ring: each
// pending op keeps its own so encoding never overwrites data a queued op
// still reads. Wrapping the ring flushes first.
static id<MTLBuffer> smallBuf(Ctx* c, size_t bytes)
{
    if (c->pendingOps >= Ctx::kSmall) c->flush();
    const int i = c->smallIdx++ % Ctx::kSmall;
    id<MTLBuffer> b = c->small[i];
    if (!b || b.length < bytes) {
        id<MTLBuffer> nb = [c->dev newBufferWithLength:std::max(bytes, (size_t)65 * 65 * 3 * sizeof(float))
                                               options:MTLResourceStorageModeShared];
        if (!nb) return nil;
#if !__has_feature(objc_arc)
        [b release];
#endif
        c->small[i] = nb;
        b = nb;
    }
    return b;
}

void gridWarp(Framebuffer& fb, const float* grid, int GX, int GY, bool wrap, bool blend)
{
    if (!grid || GX < 2 || GY < 2) return;
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    @autoreleasepool {
        const size_t bytes = (size_t)GX * GY * 3 * sizeof(float);
        id<MTLBuffer> gb = smallBuf(c, bytes);
        if (!gb) return;
        memcpy(gb.contents, grid, bytes);
        id<MTLTexture> A = c->tex[0];
        Pass p[1];
        p[0] = {@"fx_gridwarp", R, nil, A,
                {(float)GX, (float)GY, wrap ? 1.f : 0.f, blend ? 1.f : 0.f}, 4};
        p[0].buf1 = gb;
        run(c, p, 1, fb.w, fb.h, A, R);
    }
}

void shift(Framebuffer& fb, float sx, float sy, bool blend, float alpha, bool bilinear)
{
    onePass(fb, @"fx_shift", {sx, sy, blend ? 1.f : 0.f, alpha, bilinear ? 1.f : 0.f});
}

void distanceModifier(Framebuffer& fb, const float* table, int n, bool blend, bool bilinear)
{
    if (!table || n < 1) return;
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    @autoreleasepool {
        const size_t bytes = (size_t)n * sizeof(float);
        id<MTLBuffer> tb = smallBuf(c, bytes);
        if (!tb) return;
        memcpy(tb.contents, table, bytes);
        id<MTLTexture> A = c->tex[0];
        Pass p[1];
        p[0] = {@"fx_distmod", R, nil, A, {(float)n, blend ? 1.f : 0.f, bilinear ? 1.f : 0.f}, 3};
        p[0].buf1 = tb;
        run(c, p, 1, fb.w, fb.h, A, R);
    }
}

void colorOp(Framebuffer& fb, int op, const float* params, int nParams,
             const float* lut, int lutFloats)
{
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    @autoreleasepool {
        // buffer(1) is declared by the kernel, so always bind something.
        const size_t bytes = (lut && lutFloats > 0) ? (size_t)lutFloats * sizeof(float) : 16;
        id<MTLBuffer> lb = smallBuf(c, bytes);
        if (!lb) return;
        if (lut && lutFloats > 0) memcpy(lb.contents, lut, bytes);
        id<MTLTexture> A = c->tex[0];
        Pass p[1];
        p[0].kernel = @"fx_colorop"; p[0].in0 = R; p[0].out = A;
        p[0].params[0] = (float)op;
        const int n = std::min(nParams, 39);
        for (int i = 0; i < n; ++i) p[0].params[1 + i] = params ? params[i] : 0.f;
        p[0].nParams = 1 + n;
        p[0].buf1 = lb;
        run(c, p, 1, fb.w, fb.h, A, R);
    }
}

bool blend(Framebuffer& dest, const Framebuffer& src, int mode, float adjustable)
{
    if (mode == 0 || mode == 12 || src.w != dest.w || src.h != dest.h) return false;
    Ctx* c; id<MTLTexture> R = beginOp(c, dest);
    if (!R) return false;
    id<MTLTexture> S = fbTexture(c, src);
    if (!S) return false;
    @autoreleasepool {
        id<MTLTexture> A = c->tex[0];
        Pass p[1];
        p[0] = {@"fx_blend", R, S, A, {(float)mode, adjustable}, 2};
        run(c, p, 1, dest.w, dest.h, A, R);
    }
    return true;
}

void convolve5(Framebuffer& fb, const float k[25], float invScale, float bias,
               bool absolute, bool wrap, float mix)
{
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    @autoreleasepool {
        id<MTLTexture> A = c->tex[0];
        Pass p[1];
        p[0].kernel = @"fx_conv5"; p[0].in0 = R; p[0].out = A;
        float* q = p[0].params;
        q[0] = invScale; q[1] = bias; q[2] = absolute ? 1.f : 0.f; q[3] = wrap ? 1.f : 0.f; q[4] = mix;
        memcpy(q + 5, k, 25 * sizeof(float));
        p[0].nParams = 30;
        run(c, p, 1, fb.w, fb.h, A, R);
    }
}

// ---------------------------------------------------------------------------
//  Ops
// ---------------------------------------------------------------------------
void bloom(Framebuffer& fb, float threshold, float radius, float intensity, Framebuffer* overflow)
{
    // A multi-scale pyramid (the picture pass's glow chain, shared: both run
    // in order in one command buffer): luminance-threshold prefilter, box
    // downsample to a depth set by the radius, tent upsample-and-add, then
    // frame + glow through the effect's soft rolloff. With an overflow
    // buffer (HDR) the light the rolloff removed above white is kept there.
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R || !ensurePictureCompute(c) || !ensureGlow(c, fb.w, fb.h)) return;
    id<MTLTexture> OV = (overflow && overflow->w == fb.w && overflow->h == fb.h) ? fbTexture(c, *overflow) : nil;
    @autoreleasepool {
        const int L = std::clamp((int)std::lround(std::log2(std::max(2.f, radius))), 2, c->glowLevels);
        id<MTLCommandBuffer> cb = c->cb();
        const float t = std::clamp(threshold, 0.f, 4.f);
        GlowU pu = { 1.f / fb.w, 1.f / fb.h, t, -1.f };      // luminance threshold (see pic_prefilter)
        dispatch2D(cb, c->picPrefilter, R, c->glowDown[0], nil, pu);
        for (int i = 1; i < L; ++i) {
            id<MTLTexture> prev = c->glowDown[i - 1];
            GlowU d = { 1.f / prev.width, 1.f / prev.height, 0, 0 };
            dispatch2D(cb, c->picDown, prev, c->glowDown[i], nil, d);
        }
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:c->glowDown[L - 1] toTexture:c->glowUp[L - 1]];
        [bl endEncoding];
        for (int i = L - 2; i >= 0; --i) {
            id<MTLTexture> coarse = c->glowUp[i + 1];
            GlowU d = { 1.f / coarse.width, 1.f / coarse.height, 0, 0 };
            dispatch2D(cb, c->picUp, coarse, c->glowDown[i], c->glowUp[i], d);
        }
        // Combine into scratch (A = frame, B = overflow), copy both back.
        id<MTLTexture> A = c->tex[0], B = c->tex[1];
        // The pyramid sums L copies of the thresholded light, each spread
        // wider: 1/L keeps the total what the old single blur added, so a
        // preset's feedback settles where it was tuned to.
        BloomU bu = { intensity / (float)L, OV ? 1.f : 0.f, 0, 0 };
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:c->picBloomAdd];
        [e setTexture:R atIndex:0];
        [e setTexture:c->glowUp[0] atIndex:1];
        [e setTexture:A atIndex:2];
        [e setTexture:(OV ? OV : R) atIndex:3];
        [e setTexture:B atIndex:4];
        [e setBytes:&bu length:sizeof(bu) atIndex:0];
        [e dispatchThreadgroups:MTLSizeMake(((NSUInteger)fb.w + 15) / 16, ((NSUInteger)fb.h + 15) / 16, 1)
          threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [e endEncoding];
        id<MTLBlitCommandEncoder> b2 = [cb blitCommandEncoder];
        const MTLSize sz = MTLSizeMake((NSUInteger)fb.w, (NSUInteger)fb.h, 1);
        [b2 copyFromTexture:A sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:sz
                  toTexture:R destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        if (OV)
            [b2 copyFromTexture:B sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:sz
                      toTexture:OV destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b2 endEncoding];
        if (++c->pendingOps >= 64) c->flush();
    }
}

// One-pass ops: kernel R → A, copied back.
static void onePass(Framebuffer& fb, NSString* kernel, std::initializer_list<float> params)
{
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    @autoreleasepool {
        id<MTLTexture> A = c->tex[0];
        Pass p[1];
        p[0].kernel = kernel; p[0].in0 = R; p[0].out = A;
        int n = 0;
        for (float v : params) if (n < 40) p[0].params[n++] = v;
        p[0].nParams = n;
        run(c, p, 1, fb.w, fb.h, A, R);
    }
}

void kaleidoscope(Framebuffer& fb, int segments, float angle, float zoom)
{
    onePass(fb, @"fx_kaleido", {(float)segments, angle, zoom});
}

void rgbSplit(Framebuffer& fb, float amountPx, float angle)
{
    onePass(fb, @"fx_rgbsplit", {amountPx * std::cos(angle), amountPx * std::sin(angle)});
}

void toneMap(Framebuffer& fb, float exposure, float gamma, float saturation, Framebuffer* overflow)
{
    const float ig = 1.0f / std::max(0.1f, gamma);
    if (!overflow || overflow->w != fb.w || overflow->h != fb.h) {
        onePass(fb, @"fx_tonemap", {exposure, ig, saturation});
        return;
    }
    // HDR: the same result, plus the exposed light above white kept in the
    // overflow buffer (frame → A, overflow → B, both copied back).
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R || !ensurePictureCompute(c)) return;
    id<MTLTexture> OV = fbTexture(c, *overflow);
    if (!OV) return;
    @autoreleasepool {
        id<MTLCommandBuffer> cb = c->cb();
        id<MTLTexture> A = c->tex[0], B = c->tex[1];
        const float p[4] = {exposure, ig, saturation, 0};
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:c->picToneHdr];
        [e setTexture:R atIndex:0];
        [e setTexture:A atIndex:1];
        [e setTexture:OV atIndex:2];
        [e setTexture:B atIndex:3];
        [e setBytes:p length:sizeof(p) atIndex:0];
        [e dispatchThreadgroups:MTLSizeMake(((NSUInteger)fb.w + 15) / 16, ((NSUInteger)fb.h + 15) / 16, 1)
          threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [e endEncoding];
        id<MTLBlitCommandEncoder> b2 = [cb blitCommandEncoder];
        const MTLSize sz = MTLSizeMake((NSUInteger)fb.w, (NSUInteger)fb.h, 1);
        [b2 copyFromTexture:A sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:sz
                  toTexture:R destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b2 copyFromTexture:B sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:sz
                  toTexture:OV destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b2 endEncoding];
        if (++c->pendingOps >= 64) c->flush();
    }
}

void vignette(Framebuffer& fb, float inner, float outer, float strength)
{
    onePass(fb, @"fx_vignette", {inner, outer, strength});
}

void trails(Framebuffer& fb, const Framebuffer& prev, float persistence, int mode)
{
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    id<MTLTexture> P = fbTexture(c, prev);
    if (!P) return;
    @autoreleasepool {
        id<MTLTexture> A = c->tex[0];
        Pass p[1];
        p[0].kernel = @"fx_trails"; p[0].in0 = R; p[0].in1 = P; p[0].out = A;
        p[0].params[0] = persistence; p[0].params[1] = (float)mode; p[0].nParams = 2;
        run(c, p, 1, fb.w, fb.h, A, R);
    }
}

void shockwave(Framebuffer& fb, const float radii[4], float width, float strength)
{
    onePass(fb, @"fx_shockwave", {radii[0], radii[1], radii[2], radii[3], width, strength});
}

void glitch(Framebuffer& fb, float amount, float seed, float blockPx, float tearPx)
{
    onePass(fb, @"fx_glitch", {amount, seed, blockPx, tearPx});
}

void streaks(Framebuffer& fb, float threshold, float length, float intensity,
             float tintR, float tintG, float tintB)
{
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    @autoreleasepool {
        id<MTLTexture> A = c->tex[0], B = c->tex[1];
        // threshold → 3 horizontal blurs with growing sigma = long streak tail
        float s1 = std::max(2.f, length * 0.15f);
        float s2 = std::max(4.f, length * 0.4f);
        float s3 = std::max(8.f, length);
        Pass p[5];
        p[0] = {@"fx_threshold", R, nil, A, {threshold}, 1};
        p[1] = {@"fx_blur", A, nil, B, {s1, std::min(63.f, s1 * 2), 1, 0}, 4};
        p[2] = {@"fx_blur", B, nil, A, {s2, std::min(63.f, s2 * 2), 1, 0}, 4};
        p[3] = {@"fx_blur", A, nil, B, {s3, 63, 1, 0}, 4};
        p[4] = {@"fx_streak_combine", R, B, A, {intensity, tintR, tintG, tintB}, 4};
        run(c, p, 5, fb.w, fb.h, A, R);
    }
}

void lens(Framebuffer& fb, float strength)
{
    onePass(fb, @"fx_lens", {strength});
}

void radialBlur(Framebuffer& fb, float amount, int taps)
{
    onePass(fb, @"fx_radialblur", {amount, (float)taps});
}

void edges(Framebuffer& fb, float glow, float keepSource,
           float r, float g, float b)
{
    onePass(fb, @"fx_edges", {glow, keepSource, r, g, b});
}

void duotone(Framebuffer& fb, const float shadow[3], const float highlight[3], float mixAmt)
{
    onePass(fb, @"fx_duotone", {shadow[0], shadow[1], shadow[2],
                                highlight[0], highlight[1], highlight[2], mixAmt});
}

void shimmer(Framebuffer& fb, float amountPx, float scale, float t)
{
    onePass(fb, @"fx_shimmer", {amountPx, scale, t});
}

void crt(Framebuffer& fb, float curvature, float scanlines, float mask, float corner)
{
    onePass(fb, @"fx_crt", {curvature, scanlines, mask, corner});
}

// ---------------------------------------------------------------------------
//  Custom kernels (EEL→MSL pixel scripts)
// ---------------------------------------------------------------------------
bool runCustomKernel(Framebuffer& fb, const std::string& source,
                     const float* uniforms, int nUniforms, std::string* err)
{
    Ctx* c = ctx();
    if (!c || fb.w == 0) {
        if (err) *err = "Metal unavailable";
        return false;
    }

    struct Entry { id<MTLComputePipelineState> pipe; std::string error; };
    static std::unordered_map<std::string, Entry>* cache =
        new std::unordered_map<std::string, Entry>();

    auto it = cache->find(source);
    if (it == cache->end()) {
        @autoreleasepool {
            Entry e;
            NSError* nserr = nil;
            id<MTLLibrary> lib = [c->dev newLibraryWithSource:@(source.c_str())
                                                      options:nil error:&nserr];
            id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"px_main"] : nil;
            e.pipe = fn ? [c->dev newComputePipelineStateWithFunction:fn error:&nserr] : nil;
            if (!e.pipe)
                e.error = nserr ? nserr.localizedDescription.UTF8String : "kernel compile failed";
            if (cache->size() > 64) cache->clear();     // bound growth
            it = cache->emplace(source, std::move(e)).first;
        }
    }
    if (!it->second.pipe) {
        if (err) *err = it->second.error;
        return false;
    }

    Ctx* c2; id<MTLTexture> R = beginOp(c2, fb);
    if (!R) { if (err) *err = "texture bind failed"; return false; }
    @autoreleasepool {
        id<MTLTexture> A = c->tex[0];
        id<MTLCommandBuffer> cb = c->cb();
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:it->second.pipe];
        [e setTexture:R atIndex:0];
        [e setTexture:A atIndex:1];
        [e setBytes:uniforms length:(NSUInteger)std::max(1, nUniforms) * sizeof(float) atIndex:0];
        MTLSize tg = MTLSizeMake(16, 16, 1);
        MTLSize grid = MTLSizeMake(((NSUInteger)fb.w + 15) / 16, ((NSUInteger)fb.h + 15) / 16, 1);
        [e dispatchThreadgroups:grid threadsPerThreadgroup:tg];
        [e endEncoding];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:A sourceSlice:0 sourceLevel:0
              sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake((NSUInteger)fb.w, (NSUInteger)fb.h, 1)
                 toTexture:R destinationSlice:0 destinationLevel:0
         destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b endEncoding];
        c->pendingOps++;
    }
    return true;
}

}} // namespace viz::gpu
