// Classic shader library: Metal Shading Language source, compiled when the
// GPU backend starts (included as a string by core/gpu/metal/; checked
// offline by tools/check-shaders.py).
R"MSL(
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
)MSL"
