//
// SimEffects.cpp — see SimEffects.h.
//
#include "SimEffects.h"
#include "LineMode.h"
#include <algorithm>
#include <cmath>

namespace viz {

namespace {

inline float smoothstepf(float a, float b, float x)
{
    float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// The GPU's sim_hsv: v * mix(1, hue ramp, s).
inline void hsvRgb(float h, float s, float v, float rgb[3])
{
    h -= std::floor(h);
    const float o[3] = {0.f, 2.f / 3.f, 1.f / 3.f};
    for (int i = 0; i < 3; ++i) {
        float x = h + o[i];
        x -= std::floor(x);
        float k = std::clamp(std::fabs(x * 6.f - 3.f) - 1.f, 0.f, 1.f);
        rgb[i] = v * (1.f + (k - 1.f) * s);
    }
}

inline uint32_t hashu(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

inline void grad(int x, int y, int z, float g[3])
{
    uint32_t h = hashu((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u);
    float a = (float)(h & 0xffffu) * (6.2831853f / 65536.f);
    float zz = (float)(h >> 16) * (2.f / 65536.f) - 1.f;
    float r = std::sqrt(std::max(0.f, 1.f - zz * zz));
    g[0] = r * std::cos(a); g[1] = r * std::sin(a); g[2] = zz;
}

inline float fade5(float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }

} // namespace

float simNoise(float x, float y, float z)
{
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int ix = (int)fx, iy = (int)fy, iz = (int)fz;
    const float tx = x - fx, ty = y - fy, tz = z - fz;
    float n[8];
    for (int c = 0; c < 8; ++c) {
        const int dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1;
        float g[3];
        grad(ix + dx, iy + dy, iz + dz, g);
        n[c] = g[0] * (tx - dx) + g[1] * (ty - dy) + g[2] * (tz - dz);
    }
    const float u = fade5(tx), v = fade5(ty), w = fade5(tz);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    return lerp(lerp(lerp(n[0], n[1], u), lerp(n[2], n[3], u), v),
                lerp(lerp(n[4], n[5], u), lerp(n[6], n[7], u), v), w);
}

// The frame's lows / mids / highs and loudness, from the features when the
// source has them.
static void audioLevels(const VizFrame& a, float band[3], float& level)
{
    if (a.hasFeatures) {
        band[0] = a.bassAtt; band[1] = a.midAtt; band[2] = a.trebleAtt;
        level = a.level;
    } else {
        band[0] = std::min(1.f, a.bass * 1.5f);
        band[1] = std::min(1.f, a.mid * 2.f);
        band[2] = std::min(1.f, a.treble * 3.f);
        level = (band[0] + band[1] + band[2]) / 3.f;
    }
}

// ---------------------------------------------------------------------------
//  FlowParticlesEffect
// ---------------------------------------------------------------------------
FlowParticlesEffect::~FlowParticlesEffect() { gpu::simRelease(_gpu); }
bool FlowParticlesEffect::isGpu() const { return gpu::available(); }

float FlowParticlesEffect::rnd()
{
    _rng ^= _rng << 13; _rng ^= _rng >> 17; _rng ^= _rng << 5;
    return (float)(_rng & 0xFFFFFF) / (float)0x1000000;
}

void FlowParticlesEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                                 const VizFrame& a, const EffectContext& ctx)
{
    if (cur.w == 0 || cur.h == 0) return;
    const float dt = (float)std::clamp(ctx.dt, 0.0, 0.1);
    float band[3], level;
    audioLevels(a, band, level);
    _time += dt * (0.5 + 1.0 * level);            // the flow evolves faster when it's loud

    gpu::ParticleParams p;
    p.count = (int)count;
    p.dt = dt;
    p.time = (float)_time;
    p.speed = speed * (0.6f + 0.8f * level);
    p.scale = scale;
    p.follow = follow;
    p.drag = drag;
    p.push = bassPush * band[0] * band[0] * 2.f;
    p.burst = onBeat(a) ? burst : 0.f;
    p.turbulence = turbulence * band[2];
    p.lifetime = lifetime;
    p.width = width * std::max(1.f, drawQuality().widthScale);
    p.brightness = brightness;
    p.hue = hue + hueSpeed * (float)ctx.time;
    p.hueSpread = hueSpread;
    p.seed = _seed;
    p.reset = !_started;
    _started = true;
    if (gpu::available() && gpu::flowParticles(_gpu, cur, p, overflowFor(cur))) return;
    renderCpu(cur, p);
}

void FlowParticlesEffect::renderCpu(Framebuffer& cur, const gpu::ParticleParams& p)
{
    const int n = std::clamp(p.count / 40, 1500, 8000);
    const float aspect = (float)cur.w / (float)cur.h;
    auto respawn = [&](P& q) {
        q.x = (rnd() * 2.f - 1.f) * aspect; q.y = rnd() * 2.f - 1.f;
        q.px = q.x; q.py = q.y; q.vx = q.vy = 0.f;
        q.life = p.lifetime * (0.5f + rnd()); q.age = 0.f; q.tone = rnd() - 0.5f;
    };
    if ((int)_cpu.size() != n || p.reset) {
        _cpu.resize((size_t)n);
        for (auto& q : _cpu) { respawn(q); q.life *= rnd(); q.age = 1.f; }
    }
    const float W = (float)cur.w, H = (float)cur.h, hh = H * 0.5f;
    // The GPU version's light budget, spread over fewer particles.
    const float light = 0.08f * p.brightness * W * H / ((float)n * std::max(p.width, 1.f));
    const float hw = std::max(0.5f, p.width * 0.5f);
    const float sdt = std::sqrt(p.dt);
    for (auto& q : _cpu) {
        q.px = q.x; q.py = q.y;
        q.life -= p.dt; q.age += p.dt;
        if (q.life <= 0.f || std::fabs(q.x) > aspect * 1.15f || std::fabs(q.y) > 1.15f) { respawn(q); continue; }
        const float e = 0.02f, zx = q.x * p.scale, zy = q.y * p.scale, zt = p.time * 0.15f;
        const float dy = simNoise(zx, zy + e, zt) - simNoise(zx, zy - e, zt);
        const float dx = simNoise(zx + e, zy, zt) - simNoise(zx - e, zy, zt);
        const float fx = dy * (0.5f / e) * p.speed, fy = -dx * (0.5f / e) * p.speed;
        const float r = std::sqrt(q.x * q.x + q.y * q.y) + 1e-3f, rx = q.x / r, ry = q.y / r;
        const float kick = p.burst / (0.35f + r);
        q.vx += ((fx - q.vx) * p.follow + rx * p.push) * p.dt + rx * kick + (rnd() * 2.f - 1.f) * p.turbulence * sdt;
        q.vy += ((fy - q.vy) * p.follow + ry * p.push) * p.dt + ry * kick + (rnd() * 2.f - 1.f) * p.turbulence * sdt;
        const float dmp = std::exp(-p.drag * p.dt);
        q.vx *= dmp; q.vy *= dmp;
        q.x += q.vx * p.dt; q.y += q.vy * p.dt;

        const float ax = q.px * hh + W * 0.5f, ay = q.py * hh + hh;
        const float bx = q.x * hh + W * 0.5f, by = q.y * hh + hh;
        const float len = std::hypot(bx - ax, by - ay);
        const float fd = smoothstepf(0.f, 0.4f, q.age) * smoothstepf(0.f, 0.6f, q.life);
        // Capped: a few thousand particles carry what the GPU spreads over
        // a hundred thousand, and uncapped they would all clip to white.
        const float en = std::min(light * fd / std::max(len, 1.f), 1.2f);
        const float spd = std::hypot(q.vx, q.vy);
        float rgb[3];
        hsvRgb(p.hue + q.tone * p.hueSpread + spd * 0.08f, 0.8f - std::min(spd, 1.f) * 0.2f, 1.f, rgb);
        drawSegmentAA(cur, ax, ay, bx, by, hw, rgb[0] * en, rgb[1] * en, rgb[2] * en,
                      /*additive*/1, 1.f, true, true);
    }
}

// ---------------------------------------------------------------------------
//  FluidEffect
// ---------------------------------------------------------------------------
FluidEffect::~FluidEffect() { gpu::simRelease(_gpu); }
bool FluidEffect::isGpu() const { return gpu::available(); }

std::vector<gpu::FluidSplat> FluidEffect::splatsFor(const VizFrame& a, const EffectContext& ctx, float aspect)
{
    std::vector<gpu::FluidSplat> out;
    const float dt = (float)std::clamp(ctx.dt, 0.0, 0.1);
    float band[3], level;
    audioLevels(a, band, level);
    const int E = std::clamp((int)std::lround(emitters), 1, 6);
    const float baseHue = hue + hueSpeed * (float)ctx.time;
    // Emitters orbit the centre, each stirring along its path with the
    // strength of its band (lows, mids, highs in turn) and trailing dye in
    // its own hue. Forces act over the frame (x dt); the fluid keeps a
    // share of its velocity per second, so they settle, not pile up.
    for (int k = 0; k < E; ++k) {
        const float th = 6.2831853f * ((float)k / (float)E + orbit * (float)_time);
        const float ox = std::cos(th), oy = std::sin(th);
        const float en = band[k % 3];
        const float push = force * (0.2f + 1.6f * en * en) * 6.f * dt;
        gpu::FluidSplat s;
        s.x = 0.5f + 0.28f * ox / aspect; s.y = 0.5f + 0.28f * oy;
        s.vx = -oy * push - ox * push * 0.15f;
        s.vy = ox * push - oy * push * 0.15f;
        float rgb[3];
        hsvRgb(baseHue + hueSpread * (float)k / (float)E, 0.85f, 1.f, rgb);
        const float amt = dye * (0.15f + 1.5f * en * en) * dt * 4.f;
        s.r = rgb[0] * amt; s.g = rgb[1] * amt; s.b = rgb[2] * amt;
        s.radius = radius;
        out.push_back(s);
    }
    // A hit: a ring of outward pushes and a flash of pale dye somewhere
    // near the middle.
    if (onBeat(a)) {
        ++_hits;
        const uint32_t h = hashu(_hits * 2654435761u + 17u);
        const float cx = 0.3f + 0.4f * (float)(h & 0xffff) / 65535.f;
        const float cy = 0.3f + 0.4f * (float)(h >> 16) / 65535.f;
        float rgb[3];
        hsvRgb(baseHue + 0.5f * hueSpread, 0.3f, 1.f, rgb);
        for (int j = 0; j < 6; ++j) {
            const float t = 6.2831853f * (float)j / 6.f;
            gpu::FluidSplat s;
            s.x = cx + 0.03f * std::cos(t) / aspect; s.y = cy + 0.03f * std::sin(t);
            s.vx = std::cos(t) * 1.2f * force; s.vy = std::sin(t) * 1.2f * force;
            s.r = rgb[0] * 0.5f * dye; s.g = rgb[1] * 0.5f * dye; s.b = rgb[2] * 0.5f * dye;
            s.radius = radius * 0.7f;
            out.push_back(s);
        }
    }
    return out;
}

void FluidEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                         const VizFrame& a, const EffectContext& ctx)
{
    if (cur.w == 0 || cur.h == 0) return;
    const float dt = (float)std::clamp(ctx.dt, 0.0, 0.1);
    _time += dt;
    std::vector<gpu::FluidSplat> sp = splatsFor(a, ctx, (float)cur.w / (float)cur.h);
    gpu::FluidParams p;
    p.scale = scale;
    p.dt = dt;
    p.velKeep = velKeep;
    p.dyeKeep = dyeKeep;
    p.vorticity = vorticity;
    p.iterations = 20;
    p.gain = gain;
    p.mode = (int)std::lround(mode);
    p.splats = sp.data();
    p.nSplats = (int)sp.size();
    if (gpu::available() && gpu::fluid(_gpu, cur, p, overflowFor(cur))) return;
    renderCpu(cur, p);
}

// The same solver as the GPU's (sim_fl_* in GpuFx.mm) on a coarse grid.
void FluidEffect::renderCpu(Framebuffer& cur, const gpu::FluidParams& p)
{
    const int gw = std::clamp((int)(cur.w * 0.06f), 32, 128);
    const int gh = std::max(9, (int)std::lround(gw * (float)cur.h / (float)cur.w));
    const size_t N = (size_t)gw * gh;
    if (_gw != gw || _gh != gh) {
        _gw = gw; _gh = gh;
        for (auto* v : {&_vx, &_vy, &_r, &_g, &_b, &_p, &_div, &_curl}) v->assign(N, 0.f);
        for (auto& t : _tmp) t.assign(N, 0.f);
    }
    auto at = [&](int x, int y) { return (size_t)std::clamp(y, 0, gh - 1) * gw + (size_t)std::clamp(x, 0, gw - 1); };
    auto sample = [&](const std::vector<float>& f, float x, float y) {   // x, y in cells (centres at +0.5)
        x = std::clamp(x - 0.5f, 0.f, (float)(gw - 1)); y = std::clamp(y - 0.5f, 0.f, (float)(gh - 1));
        const int x0 = (int)x, y0 = (int)y;
        const int x1 = std::min(x0 + 1, gw - 1), y1 = std::min(y0 + 1, gh - 1);
        const float tx = x - x0, ty = y - y0;
        const float a0 = f[(size_t)y0 * gw + x0] + (f[(size_t)y0 * gw + x1] - f[(size_t)y0 * gw + x0]) * tx;
        const float a1 = f[(size_t)y1 * gw + x0] + (f[(size_t)y1 * gw + x1] - f[(size_t)y1 * gw + x0]) * tx;
        return a0 + (a1 - a0) * ty;
    };
    const float aspect = (float)gw / (float)gh;
    const float dt = p.dt;

    // splats
    for (int i = 0; i < p.nSplats; ++i) {
        const gpu::FluidSplat& s = p.splats[i];
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x) {
                const float dx = ((x + 0.5f) / gw - s.x) * aspect, dy = (y + 0.5f) / gh - s.y;
                const float w = std::exp(-(dx * dx + dy * dy) / std::max(s.radius * s.radius, 1e-6f));
                if (w < 1e-4f) continue;
                const size_t k = (size_t)y * gw + x;
                _vx[k] += s.vx * gh * w; _vy[k] += s.vy * gh * w;
                _r[k] += s.r * w; _g[k] += s.g * w; _b[k] += s.b * w;
            }
    }
    // vorticity confinement
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x)
            _curl[(size_t)y * gw + x] = 0.5f * ((_vy[at(x + 1, y)] - _vy[at(x - 1, y)]) - (_vx[at(x, y + 1)] - _vx[at(x, y - 1)]));
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x) {
            float fx = 0.5f * (std::fabs(_curl[at(x, y + 1)]) - std::fabs(_curl[at(x, y - 1)]));
            float fy = 0.5f * (std::fabs(_curl[at(x + 1, y)]) - std::fabs(_curl[at(x - 1, y)]));
            const float l = std::sqrt(fx * fx + fy * fy) + 1e-5f;
            const float c = _curl[(size_t)y * gw + x];
            _tmp[0][(size_t)y * gw + x] = _vx[(size_t)y * gw + x] + fx / l * p.vorticity * c * dt;
            _tmp[1][(size_t)y * gw + x] = _vy[(size_t)y * gw + x] - fy / l * p.vorticity * c * dt;
        }
    _vx.swap(_tmp[0]); _vy.swap(_tmp[1]);
    // projection
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x) {
            const size_t k = (size_t)y * gw + x;
            const float L = x > 0 ? _vx[k - 1] : -_vx[k], R = x < gw - 1 ? _vx[k + 1] : -_vx[k];
            const float T = y > 0 ? _vy[k - gw] : -_vy[k], B = y < gh - 1 ? _vy[k + gw] : -_vy[k];
            _div[k] = 0.5f * ((R - L) + (B - T));
        }
    for (int it = 0; it < std::max(1, p.iterations / 2); ++it) {
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x)
                _tmp[2][(size_t)y * gw + x] = (_p[at(x - 1, y)] + _p[at(x + 1, y)] + _p[at(x, y - 1)] + _p[at(x, y + 1)]
                                              - _div[(size_t)y * gw + x]) * 0.25f;
        _p.swap(_tmp[2]);
    }
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x) {
            const size_t k = (size_t)y * gw + x;
            _vx[k] -= 0.5f * (_p[at(x + 1, y)] - _p[at(x - 1, y)]);
            _vy[k] -= 0.5f * (_p[at(x, y + 1)] - _p[at(x, y - 1)]);
        }
    // advection
    const float vk = std::pow(std::clamp(p.velKeep, 0.f, 1.f), dt);
    const float dk = std::pow(std::clamp(p.dyeKeep, 0.f, 1.f), dt);
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x) {
            const size_t k = (size_t)y * gw + x;
            const float bx = x + 0.5f - _vx[k] * dt, by = y + 0.5f - _vy[k] * dt;
            _tmp[0][k] = sample(_vx, bx, by) * vk;
            _tmp[1][k] = sample(_vy, bx, by) * vk;
            _tmp[2][k] = sample(_r, bx, by) * dk;
            _tmp[3][k] = sample(_g, bx, by) * dk;
            _tmp[4][k] = sample(_b, bx, by) * dk;
        }
    _vx.swap(_tmp[0]); _vy.swap(_tmp[1]); _r.swap(_tmp[2]); _g.swap(_tmp[3]); _b.swap(_tmp[4]);
    // into the frame
    const float sx = (float)gw / cur.w, sy = (float)gh / cur.h;
    for (int y = 0; y < cur.h; ++y)
        for (int x = 0; x < cur.w; ++x) {
            const float gx = (x + 0.5f) * sx, gy = (y + 0.5f) * sy;
            const float r = std::max(0.f, sample(_r, gx, gy)) * p.gain;
            const float g = std::max(0.f, sample(_g, gx, gy)) * p.gain;
            const float b = std::max(0.f, sample(_b, gx, gy)) * p.gain;
            if (p.mode == 1) {
                float* px = cur.at(x, y);
                px[0] = std::min(r, 1.f); px[1] = std::min(g, 1.f); px[2] = std::min(b, 1.f); px[3] = 1.f;
            } else if (r + g + b > 1e-5f) {
                addLight(cur, x, y, r, g, b);
            }
        }
}

// ---------------------------------------------------------------------------
//  ReactionDiffusionEffect
// ---------------------------------------------------------------------------
ReactionDiffusionEffect::~ReactionDiffusionEffect() { gpu::simRelease(_gpu); }
bool ReactionDiffusionEffect::isGpu() const { return gpu::available(); }

float ReactionDiffusionEffect::rnd()
{
    _rng ^= _rng << 13; _rng ^= _rng >> 17; _rng ^= _rng << 5;
    return (float)(_rng & 0xFFFFFF) / (float)0x1000000;
}

void ReactionDiffusionEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                                     const VizFrame& a, const EffectContext& ctx)
{
    if (cur.w == 0 || cur.h == 0) return;
    const float dt = (float)std::clamp(ctx.dt, 0.0, 0.1);
    float band[3], level;
    audioLevels(a, band, level);
    // The music leans the recipe: mids toward more feed (stripes, coral),
    // highs toward more kill (spots). Slowly: patterns need seconds to
    // answer a new recipe.
    const float tf = feed + audio * 0.012f * (band[1] - 0.5f) * 2.f;
    const float tk = kill + audio * 0.003f * (band[2] - 0.5f) * 2.f;
    if (!_started) { _feedNow = tf; _killNow = tk; }
    const float k = 1.f - std::exp(-dt / 2.f);
    _feedNow += (tf - _feedNow) * k;
    _killNow += (tk - _killNow) * k;

    std::vector<gpu::RDSeed> seeds;
    const bool reset = !_started;
    if (reset)
        for (int i = 0; i < 28; ++i) seeds.push_back({rnd(), rnd(), seedSize * (0.5f + rnd())});
    if (onBeat(a))
        for (int i = 0; i < 2; ++i) seeds.push_back({0.1f + 0.8f * rnd(), 0.1f + 0.8f * rnd(), seedSize});
    _started = true;

    gpu::RDParams p;
    p.scale = scale;
    p.feed = std::clamp(_feedNow, 0.005f, 0.1f);
    p.kill = std::clamp(_killNow, 0.03f, 0.075f);
    p.iterations = std::clamp((int)std::lround(speed * (0.5f + level)), 1, 32);
    p.seeds = seeds.data();
    p.nSeeds = (int)seeds.size();
    hsvRgb(hue, 0.7f, 0.04f, p.colorA);
    hsvRgb(hue + 0.06f, 0.6f, 1.f, p.colorB);
    p.mix = mixAmt;
    p.emboss = emboss;
    p.reset = reset;
    if (gpu::available() && gpu::reactionDiffusion(_gpu, cur, p)) return;
    renderCpu(cur, p, reset);
}

// The GPU's Gray-Scott step (sim_rd_step) on a coarser grid, fewer steps.
void ReactionDiffusionEffect::renderCpu(Framebuffer& cur, const gpu::RDParams& p, bool reset)
{
    const int gw = std::clamp((int)(cur.w * p.scale / 3.f), 48, 200);
    const int gh = std::max(9, (int)std::lround(gw * (float)cur.h / (float)cur.w));
    const size_t N = (size_t)gw * gh;
    if (_gw != gw || _gh != gh || reset) {
        _gw = gw; _gh = gh;
        _u.assign(N, 1.f); _v.assign(N, 0.f); _u2.assign(N, 0.f); _v2.assign(N, 0.f);
    }
    const float aspect = (float)gw / (float)gh;
    for (int s = 0; s < p.nSeeds; ++s)
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x) {
                const float dx = ((x + 0.5f) / gw - p.seeds[s].x) * aspect, dy = (y + 0.5f) / gh - p.seeds[s].y;
                // At least two cells across, or a seed can miss every cell.
                if (std::sqrt(dx * dx + dy * dy) < std::max(p.seeds[s].radius, 2.f / gh)) {
                    _u[(size_t)y * gw + x] = 0.5f; _v[(size_t)y * gw + x] = 0.25f;
                }
            }
    auto w = [&](int x, int y) { return (size_t)((y + gh) % gh) * gw + (size_t)((x + gw) % gw); };
    for (int it = 0; it < std::clamp(p.iterations / 2, 1, 8); ++it) {
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x) {
                const size_t k = (size_t)y * gw + x;
                const float u = _u[k], v = _v[k];
                const float lu = -u + 0.2f * (_u[w(x - 1, y)] + _u[w(x + 1, y)] + _u[w(x, y - 1)] + _u[w(x, y + 1)])
                               + 0.05f * (_u[w(x - 1, y - 1)] + _u[w(x + 1, y - 1)] + _u[w(x - 1, y + 1)] + _u[w(x + 1, y + 1)]);
                const float lv = -v + 0.2f * (_v[w(x - 1, y)] + _v[w(x + 1, y)] + _v[w(x, y - 1)] + _v[w(x, y + 1)])
                               + 0.05f * (_v[w(x - 1, y - 1)] + _v[w(x + 1, y - 1)] + _v[w(x - 1, y + 1)] + _v[w(x + 1, y + 1)]);
                const float uvv = u * v * v;
                _u2[k] = std::clamp(u + p.du * lu - uvv + p.feed * (1.f - u), 0.f, 1.f);
                _v2[k] = std::clamp(v + p.dv * lv + uvv - (p.feed + p.kill) * v, 0.f, 1.f);
            }
        _u.swap(_u2); _v.swap(_v2);
    }
    auto V = [&](float x, float y) {
        const float fx = std::floor(x), fy = std::floor(y);
        const int x0 = (int)fx, y0 = (int)fy;
        const float tx = x - fx, ty = y - fy;
        const float a0 = _v[w(x0, y0)] + (_v[w(x0 + 1, y0)] - _v[w(x0, y0)]) * tx;
        const float a1 = _v[w(x0, y0 + 1)] + (_v[w(x0 + 1, y0 + 1)] - _v[w(x0, y0 + 1)]) * tx;
        return a0 + (a1 - a0) * ty;
    };
    const float sx = (float)gw / cur.w, sy = (float)gh / cur.h;
    for (int y = 0; y < cur.h; ++y)
        for (int x = 0; x < cur.w; ++x) {
            const float gx = (x + 0.5f) * sx - 0.5f, gy = (y + 0.5f) * sy - 0.5f;
            const float v = V(gx, gy);
            const float slope = (V(gx, gy + 1.f) - V(gx, gy - 1.f)) - (V(gx + 1.f, gy) - V(gx - 1.f, gy));
            const float shade = std::max(0.f, 1.f + p.emboss * 4.f * slope);
            const float t = smoothstepf(0.1f, 0.45f, v);
            float* px = cur.at(x, y);
            for (int c = 0; c < 3; ++c) {
                const float col = (p.colorA[c] + (p.colorB[c] - p.colorA[c]) * t) * shade;
                px[c] = std::clamp(px[c] + (col - px[c]) * p.mix, 0.f, 1.f);
            }
            px[3] = 1.f;
        }
}

} // namespace viz
