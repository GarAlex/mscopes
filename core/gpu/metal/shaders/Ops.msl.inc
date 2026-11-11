// Ops shader library: Metal Shading Language source, compiled when the
// GPU backend starts (included as a string by core/gpu/metal/; checked
// offline by tools/check-shaders.py).
R"MSL(
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
)MSL"
