// Sim shader library: Metal Shading Language source, compiled when the
// GPU backend starts (included as a string by core/gpu/metal/; checked
// offline by tools/check-shaders.py).
R"MSL(
#include <metal_stdlib>
using namespace metal;

// ---- randoms and noise ----
static uint sim_hashu(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
static float sim_rand(uint a, uint b) {
    return float(sim_hashu(a * 0x9E3779B9u + sim_hashu(b))) * (1.0 / 4294967296.0);
}
static float3 sim_grad(float3 i) {
    uint h = sim_hashu(uint(int(i.x)) * 73856093u ^ uint(int(i.y)) * 19349663u ^ uint(int(i.z)) * 83492791u);
    float a = float(h & 0xffffu) * (6.2831853 / 65536.0);
    float z = float(h >> 16) * (2.0 / 65536.0) - 1.0;
    float r = sqrt(max(0.0, 1.0 - z * z));
    return float3(r * cos(a), r * sin(a), z);
}
static float sim_noise(float3 p) {            // gradient noise, about -1..1
    float3 i = floor(p), f = p - i;
    float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float n000 = dot(sim_grad(i), f);
    float n100 = dot(sim_grad(i + float3(1, 0, 0)), f - float3(1, 0, 0));
    float n010 = dot(sim_grad(i + float3(0, 1, 0)), f - float3(0, 1, 0));
    float n110 = dot(sim_grad(i + float3(1, 1, 0)), f - float3(1, 1, 0));
    float n001 = dot(sim_grad(i + float3(0, 0, 1)), f - float3(0, 0, 1));
    float n101 = dot(sim_grad(i + float3(1, 0, 1)), f - float3(1, 0, 1));
    float n011 = dot(sim_grad(i + float3(0, 1, 1)), f - float3(0, 1, 1));
    float n111 = dot(sim_grad(i + float3(1, 1, 1)), f - float3(1, 1, 1));
    return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y),
               mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}
static float3 sim_hsv(float h, float s, float v) {
    float3 k = clamp(abs(fract(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    return v * mix(float3(1.0), k, s);
}

// ---- flow-field particles ----
struct Particle { float2 pos; float2 vel; float2 prev; float life; float age; };
struct PartU {
    float aspect, dt, time, speed;
    float scale, follow, drag, push;
    float burst, turbulence, lifetime, width;
    float brightness, hue, hueSpread, seed;
    float W, H; uint count, frame;
    uint reset, pad0, pad1, pad2;
};

static void sim_respawn(thread Particle& p, uint id, constant PartU& u) {
    uint s = uint(u.seed * 977.0);
    p.pos = float2((sim_rand(id, u.frame * 3u + 1u + s) * 2.0 - 1.0) * u.aspect,
                    sim_rand(id, u.frame * 3u + 2u + s) * 2.0 - 1.0);
    p.prev = p.pos;
    p.vel = float2(0.0);
    p.life = u.lifetime * (0.5 + sim_rand(id, u.frame * 3u + 3u + s));
    p.age = 0.0;
}

kernel void sim_part_step(device Particle* ps [[buffer(0)]],
                          constant PartU& u [[buffer(1)]],
                          uint id [[thread_position_in_grid]])
{
    if (id >= u.count) return;
    Particle p = ps[id];
    if (u.reset != 0u) {                       // scatter, with lives spread out
        sim_respawn(p, id, u);
        p.life *= sim_rand(id, 99u);
        p.age = 1.0;
        ps[id] = p;
        return;
    }
    p.prev = p.pos;
    p.life -= u.dt;
    p.age += u.dt;
    if (p.life <= 0.0 || abs(p.pos.x) > u.aspect * 1.15 || abs(p.pos.y) > 1.15) {
        sim_respawn(p, id, u);
        ps[id] = p;
        return;
    }
    // The flow: curl of animated noise, divergence-free, so it swirls and
    // never piles particles up.
    float3 q = float3(p.pos * u.scale, u.time * 0.15);
    const float e = 0.02;
    float dy = sim_noise(q + float3(0, e, 0)) - sim_noise(q - float3(0, e, 0));
    float dx = sim_noise(q + float3(e, 0, 0)) - sim_noise(q - float3(e, 0, 0));
    float2 flow = float2(dy, -dx) * (0.5 / e) * u.speed;
    float r = length(p.pos) + 1e-3;
    float2 radial = p.pos / r;
    float2 jit = float2(sim_rand(id, u.frame * 5u + 11u), sim_rand(id, u.frame * 5u + 12u)) * 2.0 - 1.0;
    p.vel += ((flow - p.vel) * u.follow + radial * u.push) * u.dt
           + radial * (u.burst / (0.35 + r))
           + jit * (u.turbulence * sqrt(u.dt));
    p.vel *= exp(-u.drag * u.dt);
    p.pos += p.vel * u.dt;
    ps[id] = p;
}

struct PartOut { float4 pos [[position]]; float3 col; float across; };

vertex PartOut sim_part_vs(uint vid [[vertex_id]], uint iid [[instance_id]],
                           const device Particle* ps [[buffer(0)]],
                           constant PartU& u [[buffer(1)]])
{
    Particle p = ps[iid];
    float2 halfSz = float2(u.W, u.H) * 0.5;
    float2 A = p.prev * halfSz.y + halfSz;     // height units -> pixels (y down)
    float2 B = p.pos * halfSz.y + halfSz;
    float2 d = B - A;
    float len = length(d);
    float2 dir = len > 1e-3 ? d / len : float2(1.0, 0.0);
    float2 nrm = float2(-dir.y, dir.x);
    float hw = max(0.5, u.width * 0.5) + 0.75;  // plus an anti-aliasing fringe
    const float2 corners[6] = { float2(0, -1), float2(1, -1), float2(1, 1),
                                float2(0, -1), float2(1, 1), float2(0, 1) };
    float2 cs = corners[vid];
    float2 P = mix(A, B, cs.x) + dir * (cs.x * 2.0 - 1.0) * hw + nrm * cs.y * hw;
    PartOut o;
    o.pos = float4(P.x / u.W * 2.0 - 1.0, 1.0 - P.y / u.H * 2.0, 0.0, 1.0);
    o.across = cs.y * hw / max(0.5, u.width * 0.5 + 0.25);
    // A particle's light is the same however far it moved: a streak spreads
    // it over its length. The total over all particles keeps the frame's
    // mean near 0.08 * brightness at any count and resolution. Fade in
    // after respawn, out before it.
    float fade = smoothstep(0.0, 0.4, p.age) * smoothstep(0.0, 0.6, p.life);
    float light = 0.08 * u.brightness * u.W * u.H / (float(u.count) * max(u.width, 1.0));
    float e = light * fade / max(len, 1.0);
    float tone = sim_rand(iid, 7u) - 0.5;
    float speed = length(p.vel);
    o.col = sim_hsv(u.hue + tone * u.hueSpread + speed * 0.08, 0.8 - min(speed, 1.0) * 0.2, 1.0) * e;
    return o;
}

fragment half4 sim_part_fs(PartOut in [[stage_in]])
{
    float a = saturate(1.25 - abs(in.across));
    return half4(half3(in.col * a), 1.0h);
}

// fb + light, clamped at white; the part above white goes to the overflow.
kernel void sim_add(texture2d<float, access::read> fb [[texture(0)]],
                    texture2d<float, access::sample> light [[texture(1)]],
                    texture2d<float, access::write> dst [[texture(2)]],
                    texture2d<float, access::read_write> ov [[texture(3)]],
                    constant float4& u [[buffer(0)]],          // gain, hdr, mode (0 add, 1 replace)
                    uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler lin(filter::linear, address::clamp_to_edge);
    float2 uv = (float2(g) + 0.5) / float2(dst.get_width(), dst.get_height());
    float4 c = fb.read(g);
    float3 l = max(light.sample(lin, uv).rgb, 0.0) * u.x;
    float3 x = u.z > 0.5 ? l : c.rgb + l;
    dst.write(float4(min(x, 1.0), 1.0), g);
    float3 e = max(x - 1.0, 0.0);
    if (u.y > 0.5 && any(e > 0.0)) ov.write(float4(ov.read(g).rgb + e, 1.0), g);
}

// ---- stable fluids ----
struct FlU { float dt, keep, vort, pad; };
struct Splat { float x, y, vx, vy, r, g, b, radius; };

kernel void sim_fl_advect(texture2d<float, access::sample> src [[texture(0)]],
                          texture2d<float, access::sample> vel [[texture(1)]],
                          texture2d<float, access::write> dst [[texture(2)]],
                          constant FlU& u [[buffer(0)]],
                          uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    float2 sz = float2(dst.get_width(), dst.get_height());
    float2 pos = float2(g) + 0.5;
    float2 v = vel.sample(s, pos / sz).xy;             // cells / s
    dst.write(src.sample(s, (pos - v * u.dt) / sz) * u.keep, g);
}

kernel void sim_fl_splat(texture2d<float, access::read> src [[texture(0)]],
                         texture2d<float, access::write> dst [[texture(1)]],
                         constant Splat* sp [[buffer(0)]],
                         constant float4& info [[buffer(1)]],   // n, 0 velocity / 1 dye
                         uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    float2 sz = float2(dst.get_width(), dst.get_height());
    float2 uv = (float2(g) + 0.5) / sz;
    float aspect = sz.x / sz.y;
    float4 c = src.read(g);
    for (int i = 0; i < int(info.x); ++i) {
        float2 d = (uv - float2(sp[i].x, sp[i].y)) * float2(aspect, 1.0);
        float w = exp(-dot(d, d) / max(sp[i].radius * sp[i].radius, 1e-6));
        if (info.y > 0.5) c.rgb += float3(sp[i].r, sp[i].g, sp[i].b) * w;
        else              c.xy  += float2(sp[i].vx, sp[i].vy) * sz.y * w;   // heights/s -> cells/s
    }
    dst.write(c, g);
}

static int2 sim_clampi(int2 i, int2 n) { return clamp(i, int2(0), n - 1); }

kernel void sim_fl_curl(texture2d<float, access::read> vel [[texture(0)]],
                        texture2d<float, access::write> dst [[texture(1)]],
                        uint2 g [[thread_position_in_grid]])
{
    int2 n = int2(dst.get_width(), dst.get_height());
    if (int(g.x) >= n.x || int(g.y) >= n.y) return;
    int2 i = int2(g);
    float L = vel.read(uint2(sim_clampi(i - int2(1, 0), n))).y;
    float R = vel.read(uint2(sim_clampi(i + int2(1, 0), n))).y;
    float T = vel.read(uint2(sim_clampi(i - int2(0, 1), n))).x;
    float B = vel.read(uint2(sim_clampi(i + int2(0, 1), n))).x;
    dst.write(float4(0.5 * ((R - L) - (B - T)), 0, 0, 0), g);
}

kernel void sim_fl_vort(texture2d<float, access::read> vel [[texture(0)]],
                        texture2d<float, access::read> curl [[texture(1)]],
                        texture2d<float, access::write> dst [[texture(2)]],
                        constant FlU& u [[buffer(0)]],
                        uint2 g [[thread_position_in_grid]])
{
    int2 n = int2(dst.get_width(), dst.get_height());
    if (int(g.x) >= n.x || int(g.y) >= n.y) return;
    int2 i = int2(g);
    float L = abs(curl.read(uint2(sim_clampi(i - int2(1, 0), n))).x);
    float R = abs(curl.read(uint2(sim_clampi(i + int2(1, 0), n))).x);
    float T = abs(curl.read(uint2(sim_clampi(i - int2(0, 1), n))).x);
    float B = abs(curl.read(uint2(sim_clampi(i + int2(0, 1), n))).x);
    float C = curl.read(g).x;
    float2 f = 0.5 * float2(B - T, R - L);
    f /= length(f) + 1e-5;
    f *= u.vort * C * float2(1.0, -1.0);
    float4 v = vel.read(g);
    dst.write(float4(v.xy + f * u.dt, 0, 0), g);
}

kernel void sim_fl_div(texture2d<float, access::read> vel [[texture(0)]],
                       texture2d<float, access::write> dst [[texture(1)]],
                       uint2 g [[thread_position_in_grid]])
{
    int2 n = int2(dst.get_width(), dst.get_height());
    if (int(g.x) >= n.x || int(g.y) >= n.y) return;
    int2 i = int2(g);
    float2 C = vel.read(g).xy;
    // Walls: the velocity across a wall is the mirror of the cell's own.
    float L = i.x > 0       ? vel.read(uint2(i - int2(1, 0))).x : -C.x;
    float R = i.x < n.x - 1 ? vel.read(uint2(i + int2(1, 0))).x : -C.x;
    float T = i.y > 0       ? vel.read(uint2(i - int2(0, 1))).y : -C.y;
    float B = i.y < n.y - 1 ? vel.read(uint2(i + int2(0, 1))).y : -C.y;
    dst.write(float4(0.5 * ((R - L) + (B - T)), 0, 0, 0), g);
}

kernel void sim_fl_jacobi(texture2d<float, access::read> pr [[texture(0)]],
                          texture2d<float, access::read> div [[texture(1)]],
                          texture2d<float, access::write> dst [[texture(2)]],
                          uint2 g [[thread_position_in_grid]])
{
    int2 n = int2(dst.get_width(), dst.get_height());
    if (int(g.x) >= n.x || int(g.y) >= n.y) return;
    int2 i = int2(g);
    float L = pr.read(uint2(sim_clampi(i - int2(1, 0), n))).x;
    float R = pr.read(uint2(sim_clampi(i + int2(1, 0), n))).x;
    float T = pr.read(uint2(sim_clampi(i - int2(0, 1), n))).x;
    float B = pr.read(uint2(sim_clampi(i + int2(0, 1), n))).x;
    dst.write(float4((L + R + T + B - div.read(g).x) * 0.25, 0, 0, 0), g);
}

kernel void sim_fl_grad(texture2d<float, access::read> pr [[texture(0)]],
                        texture2d<float, access::read> vel [[texture(1)]],
                        texture2d<float, access::write> dst [[texture(2)]],
                        uint2 g [[thread_position_in_grid]])
{
    int2 n = int2(dst.get_width(), dst.get_height());
    if (int(g.x) >= n.x || int(g.y) >= n.y) return;
    int2 i = int2(g);
    float L = pr.read(uint2(sim_clampi(i - int2(1, 0), n))).x;
    float R = pr.read(uint2(sim_clampi(i + int2(1, 0), n))).x;
    float T = pr.read(uint2(sim_clampi(i - int2(0, 1), n))).x;
    float B = pr.read(uint2(sim_clampi(i + int2(0, 1), n))).x;
    float2 v = vel.read(g).xy - 0.5 * float2(R - L, B - T);
    dst.write(float4(v, 0, 0), g);
}

kernel void sim_clear(texture2d<float, access::write> dst [[texture(0)]],
                      constant float4& v [[buffer(0)]],
                      uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    dst.write(v, g);
}

// ---- reaction-diffusion (Gray-Scott) ----
struct RDU {
    float du, dv, feed, kill;
    float n, emboss, mixAmt, aspect;
    float4 colA, colB;
};
struct RDSeedG { float x, y, radius, pad; };

// U, V at i + (dx, dy), wrapping: a seamless torus.
static float2 sim_rdAt(texture2d<float, access::read> t, int2 i, int dx, int dy, int2 n) {
    return t.read(uint2((i.x + dx + n.x) % n.x, (i.y + dy + n.y) % n.y)).xy;
}
// Bilinear V at grid position q (rg32Float isn't filterable everywhere).
static float sim_rdV(texture2d<float, access::read> t, float2 q, int2 n) {
    float2 f = floor(q), w = q - f;
    int2 a = int2(f);
    float v00 = sim_rdAt(t, a, 0, 0, n).y, v10 = sim_rdAt(t, a, 1, 0, n).y;
    float v01 = sim_rdAt(t, a, 0, 1, n).y, v11 = sim_rdAt(t, a, 1, 1, n).y;
    return mix(mix(v00, v10, w.x), mix(v01, v11, w.x), w.y);
}

kernel void sim_rd_step(texture2d<float, access::read> src [[texture(0)]],
                        texture2d<float, access::write> dst [[texture(1)]],
                        constant RDU& u [[buffer(0)]],
                        uint2 g [[thread_position_in_grid]])
{
    int2 n = int2(dst.get_width(), dst.get_height());
    if (int(g.x) >= n.x || int(g.y) >= n.y) return;
    int2 i = int2(g);
    float2 c = sim_rdAt(src, i, 0, 0, n);
    float2 lap = -c + 0.2 * (sim_rdAt(src, i, -1, 0, n) + sim_rdAt(src, i, 1, 0, n)
                           + sim_rdAt(src, i, 0, -1, n) + sim_rdAt(src, i, 0, 1, n))
                    + 0.05 * (sim_rdAt(src, i, -1, -1, n) + sim_rdAt(src, i, 1, -1, n)
                            + sim_rdAt(src, i, -1, 1, n) + sim_rdAt(src, i, 1, 1, n));
    float uvv = c.x * c.y * c.y;
    float2 r = c + float2(u.du * lap.x - uvv + u.feed * (1.0 - c.x),
                          u.dv * lap.y + uvv - (u.feed + u.kill) * c.y);
    dst.write(float4(clamp(r, 0.0, 1.0), 0, 0), g);
}

kernel void sim_rd_seed(texture2d<float, access::read> src [[texture(0)]],
                        texture2d<float, access::write> dst [[texture(1)]],
                        constant RDSeedG* seeds [[buffer(0)]],
                        constant RDU& u [[buffer(1)]],
                        uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    float2 sz = float2(dst.get_width(), dst.get_height());
    float2 uv = (float2(g) + 0.5) / sz;
    float2 c = src.read(g).xy;
    for (int k = 0; k < int(u.n); ++k) {
        float2 d = (uv - float2(seeds[k].x, seeds[k].y)) * float2(sz.x / sz.y, 1.0);
        if (length(d) < max(seeds[k].radius, 2.0 / sz.y))      // at least two cells across
            c = float2(0.5, 0.25 + 0.1 * sim_rand(g.x * 7919u + g.y, uint(k)));
    }
    dst.write(float4(c, 0, 0), g);
}

kernel void sim_rd_composite(texture2d<float, access::read> fb [[texture(0)]],
                             texture2d<float, access::read> rd [[texture(1)]],
                             texture2d<float, access::write> dst [[texture(2)]],
                             constant RDU& u [[buffer(0)]],
                             uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    int2 n = int2(rd.get_width(), rd.get_height());
    float2 p = (float2(g) + 0.5) / float2(dst.get_width(), dst.get_height()) * float2(n) - 0.5;
    float v = sim_rdV(rd, p, n);
    float gx = sim_rdV(rd, p + float2(1, 0), n) - sim_rdV(rd, p - float2(1, 0), n);
    float gy = sim_rdV(rd, p + float2(0, 1), n) - sim_rdV(rd, p - float2(0, 1), n);
    float shade = 1.0 + u.emboss * 4.0 * (gy - gx);     // light from the top left
    float3 col = mix(u.colA.rgb, u.colB.rgb, smoothstep(0.1, 0.45, v)) * max(shade, 0.0);
    float4 c = fb.read(g);
    dst.write(float4(clamp(mix(c.rgb, col, u.mixAmt), 0.0, 1.0), 1.0), g);
}
)MSL"
