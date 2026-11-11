// Picture shader library: Metal Shading Language source, compiled when the
// GPU backend starts (included as a string by core/gpu/metal/; checked
// offline by tools/check-shaders.py).
R"MSL(
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
// white it rolled off is also added to the overflow buffer, in place and
// only where there is any (most pixels have none).
struct BloomU { float gain; float hdr; float pad0; float pad1; };
kernel void pic_bloom_add(texture2d<float, access::read> base [[texture(0)]],
                          texture2d<float, access::sample> glow [[texture(1)]],
                          texture2d<float, access::write> dst [[texture(2)]],
                          texture2d<float, access::read_write> ov [[texture(3)]],
                          constant BloomU& u [[buffer(0)]],
                          uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler lin(filter::linear, address::clamp_to_edge);
    float2 uv = (float2(g) + 0.5) / float2(dst.get_width(), dst.get_height());
    float3 x = max(base.read(g).rgb + glow.sample(lin, uv).rgb * u.gain, 0.0);
    dst.write(float4(1.0 - exp(-x), 1.0), g);
    float3 e = max(x - 1.0, 0.0);
    if (u.hdr > 0.5 && any(e > 0.0)) ov.write(float4(ov.read(g).rgb + e, 1.0), g);
}

// The in-stack Tone Map, HDR: the same SDR result, plus the exposed light
// above white into the overflow buffer.
kernel void pic_tonemap_hdr(texture2d<float, access::read> src [[texture(0)]],
                            texture2d<float, access::write> dst [[texture(1)]],
                            texture2d<float, access::read_write> ov [[texture(2)]],
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
    float3 e = max(x - 1.0, 0.0);
    if (any(e > 0.0)) ov.write(float4(ov.read(g).rgb + e, 1.0), g);
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
)MSL"
