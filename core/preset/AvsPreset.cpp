//
// AvsPreset.cpp — see AvsPreset.h. Format per grandchild/vis_avs (BSD-3).
//
#include "AvsPreset.h"
#include "Presets.h"
#include "Superscope.h"
#include "DynamicMovement.h"
#include "ScriptedTrans.h"
#include "DelayEffects.h"
#include "BuiltinEffects.h"
#include "TransColor.h"
#include "TransFilter.h"
#include "UtilEffects.h"
#include "EffectList.h"
#include "RenderEffects.h"

#include <functional>
#include <set>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace viz {

static constexpr const char* kMagic02 = "Nullsoft AVS Preset 0.2\x1a";
static constexpr const char* kMagic01 = "Nullsoft AVS Preset 0.1\x1a";
static constexpr int kMagicLen = 24;
static constexpr uint32_t kDllRenderBase = 16384;
static constexpr int kApeIdLen = 32;

// ---------------------------------------------------------------------------
//  Little-endian reader
// ---------------------------------------------------------------------------
namespace {
struct Reader {
    const uint8_t* p;
    size_t len, pos = 0;
    Reader(const uint8_t* d, size_t l) : p(d), len(l) {}

    bool has(size_t n) const { return pos + n <= len; }
    uint8_t  u8()  { return has(1) ? p[pos++] : 0; }
    int32_t  i32() {
        if (!has(4)) { pos = len; return 0; }
        int32_t v; std::memcpy(&v, p + pos, 4); pos += 4;   // file is LE, as is macOS
        return v;
    }
    // AVS legacy string: uint32 size + bytes (trailing NUL stripped).
    std::string str() {
        if (!has(4)) { pos = len; return {}; }
        uint32_t size = (uint32_t)i32();
        if (size == 0 || !has(size)) { if (size) pos = len; return {}; }
        std::string s((const char*)(p + pos), size);
        pos += size;
        size_t nul = s.find('\0');
        if (nul != std::string::npos) s.resize(nul);
        return s;
    }
    void skip(size_t n) { pos = std::min(len, pos + n); }
};
} // namespace

// ---------------------------------------------------------------------------
//  Legacy component ID → name (for reporting) — from vis_avs e_*.h legacy_id.
// ---------------------------------------------------------------------------
static const char* legacyName(int id)
{
    switch (id) {
        case 0: return "Simple"; case 1: return "Dot Plane";
        case 2: return "Oscilloscope Star"; case 3: return "Fadeout";
        case 4: return "Blitter Feedback"; case 5: return "OnBeat Clear";
        case 6: return "Blur"; case 7: return "Bass Spin";
        case 8: return "Moving Particle"; case 9: return "Roto Blitter";
        case 10: return "SVP"; case 11: return "Colorfade";
        case 12: return "Color Clip"; case 13: return "Rotating Stars";
        case 14: return "Ring"; case 15: return "Movement";
        case 16: return "Scatter"; case 17: return "Dot Grid";
        case 18: return "Buffer Save"; case 19: return "Dot Fountain";
        case 20: return "Water"; case 21: return "Comment";
        case 22: return "Brightness"; case 23: return "Interleave";
        case 24: return "Grain"; case 25: return "Clear Screen";
        case 26: return "Mirror"; case 27: return "Starfield";
        case 28: return "Text"; case 29: return "Bump";
        case 30: return "Mosaic"; case 31: return "Water Bump";
        case 32: return "Video"; case 33: return "Custom BPM";
        case 34: return "Picture"; case 35: return "Dynamic Distance Modifier";
        case 36: return "SuperScope"; case 37: return "Invert";
        case 38: return "Unique Tone"; case 39: return "Timescope";
        case 40: return "Set Render Mode"; case 41: return "Interferences";
        case 42: return "Dynamic Shift"; case 43: return "Dynamic Movement";
        case 44: return "Fast Brightness"; case 45: return "Color Modifier";
        default: return "Unknown";
    }
}

// Non-scripted effects we can instantiate directly (defaults or coarse config).
static const char* registryKeyForLegacyId(int id)
{
    switch (id) {
        case 0:  return "simple_scope";
        case 1:  return "dot_plane";
        case 2:  return "osc_star";
        case 4:  return "roto_blitter";     // blitter feedback ≈ roto blitter zoom
        case 7:  return "bass_spin";
        case 8:  return nullptr;            // moving particle (not ported yet)
        case 9:  return "roto_blitter";
        case 11: return "colorfade";
        case 12: return "color_clip";
        case 13: return "rot_star";
        case 15: return "movement";
        case 14: return "ring";
        case 16: return "scatter";
        case 17: return "dot_grid";
        case 19: return "dot_fountain";
        case 22: return "brightness";
        case 23: return "interleave";
        case 24: return "grain";
        case 26: return "mirror";
        case 27: return "starfield";
        case 29: return "bump";
        case 30: return "mosaic";
        case 31: return "water_bump";
        case 38: return "unique_tone";
        case 39: return "timescope";
        case 41: return "interferences";
        case 44: return "brightness";       // fastbright ≈ brightness boost
        default: return nullptr;
    }
}

// APE (named) components → registry key. IDs are the 32-byte strings AVS used.
static const char* registryKeyForApeId(const char* apeId)
{
    struct { const char* ape; const char* key; } map[] = {
        {"Channel Shift", "channel_shift"},
        {"Color Map", "color_map"},
        {"Color Reduction", "color_reduction"},
        {"Multiplier", "multiplier"},
        {"Holden04: Video Delay", nullptr},
        {"Holden05: Multi Delay", nullptr},
        {"Convolution Filter", "convolution"},
        {"Holden03: Convolution Filter", "convolution"},
        {"Jheriko : MULTIFILTER", "multi_filter"},
        {"Texer", "texer2"},                    // Texer v1 ≈ Texer II defaults
        {"Trans: Multiplier", "multiplier"},
        {"Virtual Effect: Addborders", "add_borders"},
        {"Jheriko: Global", nullptr},
        {"Jheriko : NORMALISE", "normalise"},
        {"Trans: Normalise", "normalise"},
        {"Multi Filter", "multi_filter"},
        {"Winamp Starfield v1", "starfield"},
        {"Winamp Mosaic v1", "mosaic"},
        {"Winamp Grain v1", "grain"},
        {"Winamp Interleave v1", "interleave"},
        {"Winamp ClearScreen v1", "clear_screen"},
        {"Winamp Brightness v1", "brightness"},
        {"Winamp Interferences v1", "interferences"},
        {"Winamp AVIAPE v1", nullptr},
        {"Nullsoft MIDI v1", nullptr},
        {"Nullsoft Picture II v1", nullptr},
    };
    for (const auto& m : map)
        if (!std::strncmp(apeId, m.ape, std::strlen(m.ape))) return m.key;
    return nullptr;
}

// ---------------------------------------------------------------------------
//  Script blob: version byte + 4 length-prefixed strings, or ancient fixed
//  256-byte buffers. Order is effect-specific (superscope & DM use
//  point, frame, beat, init).
// ---------------------------------------------------------------------------
static void loadScriptBlock(Reader& r, size_t end,
                            std::string& point, std::string& frame,
                            std::string& beat, std::string& init)
{
    if (r.pos < end && r.p[r.pos] == 1) {
        r.skip(1);
        point = r.str(); frame = r.str(); beat = r.str(); init = r.str();
    } else if (end - r.pos >= 1024) {
        auto fixed = [&](size_t off) {
            const char* s = (const char*)(r.p + r.pos + off);
            size_t n = strnlen(s, 256);
            return std::string(s, n);
        };
        point = fixed(0); frame = fixed(256); beat = fixed(512); init = fixed(768);
        r.skip(1024);
    }
}

// ---------------------------------------------------------------------------
//  Per-effect decoders
// ---------------------------------------------------------------------------
static std::unique_ptr<Effect> decodeSuperscope(Reader& r, size_t end)
{
    auto fx = std::make_unique<SuperscopeEffect>();
    std::string point, frame, beat, init;
    loadScriptBlock(r, end, point, frame, beat, init);
    fx->setScripts(std::move(init), std::move(frame), std::move(beat), std::move(point));
    if (end - r.pos >= 4) {
        uint32_t cs = (uint32_t)r.i32();
        switch (cs & 0b011) {                 // AVS channel encoding
            default: case 2: fx->channel = 0; break;   // center
            case 0: fx->channel = 1; break;            // left
            case 1: fx->channel = 2; break;            // right
        }
        fx->source = (cs & 0b100) ? 1.f : 0.f;
    }
    // remaining fields (color list, draw mode) intentionally ignored in v1
    return fx;
}

static std::unique_ptr<Effect> decodeDynamicMovement(Reader& r, size_t end)
{
    auto fx = std::make_unique<DynamicMovementEffect>();
    std::string point, frame, beat, init;
    loadScriptBlock(r, end, point, frame, beat, init);
    fx->setScripts(std::move(init), std::move(frame), std::move(beat), std::move(point));
    if (end - r.pos >= 4) r.i32();                        // bilinear (always on here)
    if (end - r.pos >= 4) fx->rect  = r.i32() ? 1.f : 0.f;
    if (end - r.pos >= 4) fx->gridW = (float)std::max(2, std::min(64, r.i32() + 1));
    if (end - r.pos >= 4) fx->gridH = (float)std::max(2, std::min(64, r.i32() + 1));
    if (end - r.pos >= 4) fx->blend = r.i32() ? 1.f : 0.f;
    if (end - r.pos >= 4) fx->wrap  = r.i32() ? 1.f : 0.f;
    return fx;
}

static std::unique_ptr<Effect> decodeBlur(Reader& r, size_t end)
{
    auto fx = std::make_unique<BlurEffect>();
    int level = (end - r.pos >= 4) ? r.i32() : 1;
    switch (level) {                     // legacy: 0=off 1=medium 2=light 3=heavy
        case 0: fx->enabled = false; break;
        case 2: fx->radius = 1; fx->mix = 0.5f; break;
        case 3: fx->radius = 2; fx->mix = 1.0f; break;
        default: fx->radius = 1; fx->mix = 1.0f; break;
    }
    return fx;
}

// Set Render Mode: one int — bit 31 enabled, bits 0-7 blend mode, bits 8-15
// adjustable alpha (0-255), bits 16-23 line width (0 = 1).
static std::unique_ptr<Effect> decodeSetRenderMode(Reader& r, size_t end)
{
    auto fx = std::make_unique<SetRenderModeEffect>();
    uint32_t m = (end - r.pos >= 4) ? (uint32_t)r.i32() : 0x80000001u;
    fx->enabled   = (m & 0x80000000u) != 0;
    fx->blend     = (float)std::min<uint32_t>(m & 0xffu, 9u);
    fx->alpha     = (float)((m >> 8) & 0xffu) / 255.f;
    fx->lineWidth = (float)std::max<uint32_t>((m >> 16) & 0xffu, 1u);
    return fx;
}

static std::unique_ptr<Effect> decodeFadeout(Reader& r, size_t end)
{
    auto fx = std::make_unique<FadeoutEffect>();
    if (end - r.pos >= 4) {
        int fadelen = r.i32();                            // 0..92
        fx->setParam("speed", (float)fadelen / 255.f);
    }
    if (end - r.pos >= 4) {
        uint32_t c = (uint32_t)r.i32();                   // 0x00BBGGRR? AVS colors are 0xRRGGBB in file... keep BGR read
        fx->setParam("target_b", ((c >> 16) & 0xff) / 255.f);
        fx->setParam("target_g", ((c >> 8) & 0xff) / 255.f);
        fx->setParam("target_r", (c & 0xff) / 255.f);
    }
    return fx;
}

static std::unique_ptr<Effect> decodeInvert(Reader& r, size_t end)
{
    auto fx = std::make_unique<InvertEffect>();
    if (end - r.pos >= 4) fx->enabled = r.i32() != 0;
    return fx;
}

static std::unique_ptr<Effect> decodeWater(Reader& r, size_t end)
{
    auto fx = std::make_unique<WaterEffect>();
    if (end - r.pos >= 4) fx->enabled = r.i32() != 0;
    return fx;
}

static std::unique_ptr<Effect> decodeOnBeatClear(Reader& r, size_t end)
{
    auto fx = std::make_unique<OnBeatClearEffect>();
    if (end - r.pos >= 4) {
        uint32_t c = (uint32_t)r.i32();
        fx->b = ((c >> 16) & 0xff) / 255.f;
        fx->g = ((c >> 8) & 0xff) / 255.f;
        fx->r = (c & 0xff) / 255.f;
    }
    if (end - r.pos >= 4) fx->blend = r.i32() ? 1.f : 0.f;
    if (end - r.pos >= 4) fx->everyNBeats = (float)std::max(1, r.i32());
    return fx;
}

static std::unique_ptr<Effect> decodeClearScreen(Reader& r, size_t end)
{
    // Legacy layout: enabled, color, blend, blend_5050, only_first (all
    // int32; older files may omit the trailing fields). only_first matters a
    // lot: it's how presets seed a feedback chain once and then let warp
    // effects accumulate forever instead of being wiped every frame.
    auto fx = std::make_unique<ClearScreenEffect>();
    if (end - r.pos >= 4) fx->enabled = r.i32() != 0;
    if (end - r.pos >= 4) {
        uint32_t c = (uint32_t)r.i32();
        fx->b = ((c >> 16) & 0xff) / 255.f;
        fx->g = ((c >> 8) & 0xff) / 255.f;
        fx->r = (c & 0xff) / 255.f;
    }
    if (end - r.pos >= 4) r.i32();                          // blend mode (additive unsupported)
    if (end - r.pos >= 4) fx->blend = r.i32() ? 1.f : 0.f;  // blend_5050 overrides
    if (end - r.pos >= 4) fx->onlyFirst = r.i32() ? 1.f : 0.f;
    return fx;
}

static std::unique_ptr<Effect> decodeMovingParticle(Reader& r, size_t end)
{
    auto fx = std::make_unique<MovingParticleEffect>();
    if (end - r.pos >= 4) {
        int e = r.i32();
        fx->enabled = (e & 1) != 0;
        fx->onBeatSizeChange = (e & 2) ? 1.f : 0.f;
    }
    if (end - r.pos >= 4) {
        uint32_t c = (uint32_t)r.i32();
        fx->colB = ((c >> 16) & 0xff) / 255.f;
        fx->colG = ((c >> 8) & 0xff) / 255.f;
        fx->colR = (c & 0xff) / 255.f;
    }
    if (end - r.pos >= 4) fx->distance   = (float)std::max(1, r.i32());
    if (end - r.pos >= 4) fx->size       = (float)std::max(1, r.i32());
    if (end - r.pos >= 4) fx->onBeatSize = (float)std::max(1, r.i32());
    if (end - r.pos >= 4) fx->blendMode  = (float)r.i32();
    return fx;
}

static std::unique_ptr<Effect> decodeColorModifier(Reader& r, size_t end)
{
    auto fx = std::make_unique<ColorModifierEffect>();
    std::string point, frame, beat, init;
    loadScriptBlock(r, end, point, frame, beat, init);
    fx->setScripts(std::move(init), std::move(frame), std::move(beat), std::move(point));
    if (end - r.pos >= 4) fx->recompute = r.i32() ? 1.f : 0.f;
    return fx;
}

static std::unique_ptr<Effect> decodeDynamicShift(Reader& r, size_t end)
{
    auto fx = std::make_unique<DynamicShiftEffect>();
    // dshift stores only 3 scripts, order init, frame, beat
    std::string init, frame, beat;
    if (r.pos < end && r.p[r.pos] == 1) {
        r.skip(1);
        init = r.str(); frame = r.str(); beat = r.str();
    }
    fx->setScripts(std::move(init), std::move(frame), std::move(beat));
    if (end - r.pos >= 4) fx->blend    = r.i32() ? 1.f : 0.f;
    if (end - r.pos >= 4) fx->bilinear = r.i32() ? 1.f : 0.f;
    return fx;
}

static std::unique_ptr<Effect> decodeDynamicDistance(Reader& r, size_t end)
{
    auto fx = std::make_unique<DynamicDistanceModifierEffect>();
    std::string point, frame, beat, init;
    loadScriptBlock(r, end, point, frame, beat, init);
    fx->setScripts(std::move(init), std::move(frame), std::move(beat), std::move(point));
    if (end - r.pos >= 4) fx->blend    = r.i32() ? 1.f : 0.f;
    if (end - r.pos >= 4) fx->bilinear = r.i32() ? 1.f : 0.f;
    return fx;
}

static std::unique_ptr<Effect> decodeBufferSave(Reader& r, size_t end)
{
    auto fx = std::make_unique<BufferSaveEffect>();
    if (end - r.pos >= 4) fx->action = (float)std::clamp(r.i32(), 0, 3);
    if (end - r.pos >= 4) fx->buffer = (float)std::clamp(r.i32(), 0, 7);
    if (end - r.pos >= 4) fx->blendMode = (float)std::clamp(r.i32(), 0, 11);
    if (end - r.pos >= 4) fx->adjustable = std::clamp(r.i32(), 0, 255) / 255.f;
    return fx;
}

static std::unique_ptr<Effect> decodeGlobalVariables(Reader& r, size_t end)
{
    auto fx = std::make_unique<GlobalVariablesEffect>();
    if (end - r.pos >= 4) r.i32();                 // file load mode (unsupported)
    r.skip(24);                                    // reserved
    auto cstr = [&]() {                            // null-terminated string
        std::string s;
        while (r.pos < end && r.p[r.pos]) s.push_back((char)r.p[r.pos++]);
        if (r.pos < end) r.pos++;                  // consume the NUL
        return s;
    };
    std::string init = cstr(), frame = cstr(), beat = cstr();
    fx->setScripts(std::move(init), std::move(frame), std::move(beat));
    // filename + reg/buf save ranges (file persistence) intentionally ignored
    return fx;
}

static std::unique_ptr<Effect> decodeCustomBPM(Reader& r, size_t end)
{
    auto fx = std::make_unique<CustomBPMEffect>();
    bool en = true;
    if (end - r.pos >= 4) en = r.i32() != 0;
    fx->enabled = en;
    bool arb = false, skip = false, inv = false;
    if (end - r.pos >= 4) arb  = r.i32() != 0;
    if (end - r.pos >= 4) skip = r.i32() != 0;
    if (end - r.pos >= 4) inv  = r.i32() != 0;
    fx->mode = arb ? 0.f : (skip ? 1.f : (inv ? 2.f : 0.f));
    if (end - r.pos >= 4) {
        int ms = r.i32();                             // beat interval in ms
        if (ms > 0) fx->bpm = std::clamp(60000.f / ms, 10.f, 400.f);
    }
    if (end - r.pos >= 4) fx->skip = (float)std::clamp(r.i32(), 1, 16);
    if (end - r.pos >= 4) fx->skipFirst = (float)std::clamp(r.i32(), 0, 64);
    return fx;
}

static std::unique_ptr<Effect> decodeVideoDelay(Reader& r, size_t end)
{
    auto fx = std::make_unique<VideoDelayEffect>();
    if (end - r.pos >= 4) fx->enabled = r.i32() != 0;
    if (end - r.pos >= 4) fx->useBeats = r.i32() ? 1.f : 0.f;
    if (end - r.pos >= 4) fx->delay = (float)std::clamp(r.i32(), 0, 200);
    return fx;
}

static std::unique_ptr<Effect> decodeMultiDelay(Reader& r, size_t end)
{
    auto fx = std::make_unique<MultiDelayEffect>();
    if (end - r.pos >= 4) fx->mode = (float)std::clamp(r.i32(), 0, 2);
    int active = 0;
    if (end - r.pos >= 4) active = std::clamp(r.i32(), 0, 5);
    fx->buffer = (float)active;
    for (int i = 0; i < 6 && end - r.pos >= 8; ++i) {   // per-buffer configs
        int ub = r.i32(), dl = r.i32();
        if (i == active) {
            fx->useBeats = ub ? 1.f : 0.f;
            fx->delay = (float)std::clamp(dl, 0, 200);
        }
    }
    return fx;
}

static std::unique_ptr<Effect> decodeTriangle(Reader& r, size_t end)
{
    auto fx = std::make_unique<TriangleEffect>();
    auto cstr = [&]() {
        std::string s;
        while (r.pos < end && r.p[r.pos]) s.push_back((char)r.p[r.pos++]);
        if (r.pos < end) r.pos++;
        return s;
    };
    std::string init = cstr(), frame = cstr(), beat = cstr(), point = cstr();
    fx->setScripts(std::move(init), std::move(frame), std::move(beat), std::move(point));
    return fx;
}

static std::unique_ptr<Effect> decodeTexer2(Reader& r, size_t end)
{
    auto fx = std::make_unique<TexerIIEffect>();
    if (end - r.pos >= 4) r.i32();                 // version
    if (end - r.pos >= 260) {                      // image path (MAX_PATH chars)
        const char* p = (const char*)(r.p + r.pos);
        std::string path(p, strnlen(p, 260));
        size_t slash = path.find_last_of("/\\");
        if (slash != std::string::npos) path = path.substr(slash + 1);
        if (!path.empty()) fx->setImage(std::move(path));
        r.skip(260);
    }
    if (end - r.pos >= 4) r.i32();                 // resize flag (we always scale)
    if (end - r.pos >= 4) fx->wrap = r.i32() ? 1.f : 0.f;
    if (end - r.pos >= 4) fx->colorize = r.i32() ? 1.f : 0.f;
    if (end - r.pos >= 4) r.i32();                 // unused
    std::string init = r.str(), frame = r.str(), beat = r.str(), point = r.str();
    fx->setScripts(std::move(init), std::move(frame), std::move(beat), std::move(point));
    return fx;
}

// ---------------------------------------------------------------------------
//  Component list body (root and nested effect lists share this layout).
//  `list` is null for the root (whose mode byte config we ignore); for nested
//  lists the decoded blend config is stored on it.
// ---------------------------------------------------------------------------
using AddEffect = std::function<void(std::unique_ptr<Effect>)>;

static void parseListBody(Reader& r, size_t end, const AddEffect& add,
                          AvsLoadReport& report, int depth,
                          EffectListEffect* list)
{
    if (depth > 8) return;                    // sanity

    // list mode byte(s) — see vis_avs e_effectlist.cpp load_legacy.
    // The extended-config fields are read one by one, each guarded against
    // `ext`; children start wherever that leaves off (NOT at ext — the last
    // two on-beat fields are guarded by ext-4 and often absent).
    size_t bodyStart = r.pos;
    uint8_t first = r.u8();
    bool listEnabled = !(first & 0b10);
    bool clearEveryFrame = (first & 0b01) != 0;
    int inputBlend = 1, outputBlend = 1, ext = 0;
    if (first & 0x80) {
        uint8_t m0 = r.u8(); (void)m0;
        uint8_t m1 = r.u8();
        uint8_t m2 = r.u8();
        uint8_t m3 = r.u8();
        inputBlend = m1 & 0b111111;
        outputBlend = (m2 & 0b111111) ^ 1;    // stored flipped, see reference
        ext = (int)m3 + 5;
    }
    // Extended fields, in order: input/output adjustable amount (0..255),
    // input/output buffer slot (1-indexed; 0 = none), then invert flags and
    // on-beat fields we don't use yet.
    int inAdj = 128, outAdj = 128, inBuf = 0, outBuf = 0;
    if (ext > 5) {
        size_t pos = r.pos - bodyStart;       // 5 after the mode bytes
        int* capture[6] = {&inAdj, &outAdj, &inBuf, &outBuf, nullptr, nullptr};
        for (int k = 0; k < 6 && pos < (size_t)ext; ++k) {
            int v = r.i32(); pos += 4;
            if (capture[k]) *capture[k] = v;
        }
        for (int k = 0; k < 2 && pos + 4 < (size_t)ext; ++k) { r.i32(); pos += 4; }
        r.pos = std::min(end, bodyStart + pos);
    }
    if (list) {
        list->enabled = listEnabled;
        list->clearEveryFrame = clearEveryFrame ? 1.f : 0.f;
        list->inputBlend = (float)inputBlend;
        list->outputBlend = (float)outputBlend;
        list->inputAdjustable = std::clamp(inAdj, 0, 255) / 255.f;
        list->outputAdjustable = std::clamp(outAdj, 0, 255) / 255.f;
        list->inputBlendBuffer = (float)(std::clamp(inBuf, 0, 8) - 1);
        list->outputBlendBuffer = (float)(std::clamp(outBuf, 0, 8) - 1);
        if (getenv("AVS_DEBUG"))
            fprintf(stderr, "[avs] list depth=%d enabled=%d clear=%d in=%d out=%d ext=%d "
                            "inbuf=%d outbuf=%d\n",
                    depth, listEnabled, clearEveryFrame, inputBlend, outputBlend, ext,
                    inBuf - 1, outBuf - 1);
    }

    while (r.pos + 8 <= end) {
        int id = r.i32();
        char apeId[kApeIdLen + 1] = {0};
        if ((uint32_t)id >= kDllRenderBase && id != -2) {
            if (r.pos + kApeIdLen > end) break;
            std::memcpy(apeId, r.p + r.pos, kApeIdLen);
            r.skip(kApeIdLen);
        }
        int compLen = r.i32();
        if (compLen < 0 || r.pos + (size_t)compLen > end) break;
        size_t compEnd = r.pos + (size_t)compLen;

        if (id == -2) {
            // nested effect list → group effect with real blend semantics
            auto sub = std::make_unique<EffectListEffect>();
            EffectListEffect* subPtr = sub.get();
            parseListBody(r, compEnd,
                          [subPtr](std::unique_ptr<Effect> e) { subPtr->addChild(std::move(e)); },
                          report, depth + 1, subPtr);
            report.loaded.push_back("Effect List");
            add(std::move(sub));          // empty lists are valid no-ops
        } else if ((uint32_t)id >= kDllRenderBase &&
                   !std::strncmp(apeId, "AVS 2.8+ Effect List Config", 27)) {
            // the enclosing list's use_code flag + init/frame scripts —
            // we don't run list code yet; consume without reporting
        } else if ((uint32_t)id >= kDllRenderBase &&
                   !std::strncmp(apeId, "Jheriko: Global", 15)) {
            add(decodeGlobalVariables(r, compEnd));
            report.loaded.push_back("Global Variables");
        } else if ((uint32_t)id >= kDllRenderBase &&
                   !std::strncmp(apeId, "Render: Triangle", 16)) {
            add(decodeTriangle(r, compEnd));
            report.loaded.push_back("Triangle");
        } else if ((uint32_t)id >= kDllRenderBase &&
                   !std::strncmp(apeId, "Acko.net: Texer II", 18)) {
            add(decodeTexer2(r, compEnd));
            report.loaded.push_back("Texer II");
        } else if ((uint32_t)id >= kDllRenderBase &&
                   !std::strncmp(apeId, "Holden04: Video Delay", 21)) {
            add(decodeVideoDelay(r, compEnd));
            report.loaded.push_back("Video Delay");
        } else if ((uint32_t)id >= kDllRenderBase &&
                   !std::strncmp(apeId, "Holden05: Multi Delay", 21)) {
            add(decodeMultiDelay(r, compEnd));
            report.loaded.push_back("Multi Delay");
        } else if ((uint32_t)id >= kDllRenderBase) {
            const char* key = registryKeyForApeId(apeId);
            if (key) {
                auto it = effectRegistry().find(key);
                if (it != effectRegistry().end()) {
                    add(it->second());
                    report.partial.push_back(std::string("APE ") + apeId);
                }
            } else {
                report.skipped.push_back(std::string("APE ") + apeId);
            }
        } else {
            std::unique_ptr<Effect> fx;
            bool full = true;
            switch (id) {
                case 36: fx = decodeSuperscope(r, compEnd); break;
                case 43: fx = decodeDynamicMovement(r, compEnd); break;
                case 6:  fx = decodeBlur(r, compEnd); break;
                case 3:  fx = decodeFadeout(r, compEnd); break;
                case 37: fx = decodeInvert(r, compEnd); break;
                case 20: fx = decodeWater(r, compEnd); break;
                case 5:  fx = decodeOnBeatClear(r, compEnd); break;
                case 25: fx = decodeClearScreen(r, compEnd); break;
                case 8:  fx = decodeMovingParticle(r, compEnd); break;
                case 45: fx = decodeColorModifier(r, compEnd); break;
                case 42: fx = decodeDynamicShift(r, compEnd); break;
                case 35: fx = decodeDynamicDistance(r, compEnd); break;
                case 18: fx = decodeBufferSave(r, compEnd); break;
                case 33: fx = decodeCustomBPM(r, compEnd); break;
                case 40: fx = decodeSetRenderMode(r, compEnd); break;
                default: {
                    const char* key = registryKeyForLegacyId(id);
                    if (key) {
                        auto it = effectRegistry().find(key);
                        if (it != effectRegistry().end()) {
                            fx = it->second();
                            full = false;
                        }
                    }
                    break;
                }
            }
            if (fx) {
                (full ? report.loaded : report.partial).push_back(legacyName(id));
                if (getenv("AVS_DEBUG")) fprintf(stderr, "[avs]   depth=%d comp=%s\n", depth, legacyName(id));
                add(std::move(fx));
            } else {
                report.skipped.push_back(legacyName(id));
            }
        }
        r.pos = compEnd;                       // always resync to component end
    }
    r.pos = end;
}

// ---------------------------------------------------------------------------
//  Fragment detection: presets whose stack draws no content of its own
//  (transform/color-only) are "modifier" presets — they were authored to
//  work on whatever image is already on screen, so they inherit the canvas.
// ---------------------------------------------------------------------------
static bool rendersContent(Effect* e)
{
    // Deliberately EXCLUDES Clear Screen / OnBeat Clear: a uniform fill
    // gives warp/feedback effects (Movement, Dynamic Movement, Interleave,
    // Interferences, Mirror, Water, ...) zero spatial detail to work with —
    // a "Clear + warps only" stack is visually degenerate (flat/near-black)
    // starting from scratch, same as a true content-free fragment. Such
    // stacks are almost always feedback-only presets meant to recycle
    // whatever's already on screen, so they should inherit the canvas too.
    static const std::set<std::string> sources = {
        "Superscope", "Simple Scope", "Scope", "Ring", "Oscilloscope Star",
        "Rotating Stars", "Bass Spin", "Dot Grid", "Dot Plane", "Dot Fountain",
        "Timescope", "Moving Particle", "Starfield", "Texer II", "Triangle",
        "Particles", "Spectrum Bars", "Pixel Shader",
    };
    if (sources.count(e->name())) return true;
    if (auto* list = dynamic_cast<EffectListEffect*>(e))
        for (size_t i = 0; i < list->childCount(); ++i)
            if (rendersContent(list->childAt(i))) return true;
    return false;
}

static bool stackRendersContent(EffectHost& host)
{
    for (size_t i = 0; i < host.count(); ++i)
        if (rendersContent(host.at(i))) return true;
    return false;
}

// ---------------------------------------------------------------------------
//  Entry points
// ---------------------------------------------------------------------------
bool loadAvsPreset(EffectHost& host, const uint8_t* data, size_t len,
                   AvsLoadReport& report, bool forceInherit)
{
    if (len < (size_t)kMagicLen ||
        (std::memcmp(data, kMagic02, kMagicLen) != 0 &&
         std::memcmp(data, kMagic01, kMagicLen) != 0)) {
        report.error = "not an AVS preset (bad magic)";
        return false;
    }
    resetSharedState();
    host.clearEffects(/*clearCanvas=*/false);   // decided after parsing

    // The whole preset is itself an (implicit) effect list — same clear/
    // input-blend/output-blend machinery as any nested list, operating
    // directly on the host's persistent screen buffer. This matters: a
    // "clear every frame" root with the (common, pre-extended-config)
    // default input blend of Replace has that clear immediately undone by
    // the previous frame's content blending back in — clearing and Replace
    // input together are a no-op, NOT a forced wipe. An earlier version of
    // this loader special-cased root clearing as an unconditional black
    // Clear Screen and got exactly this wrong, permanently blacking out any
    // preset that paired clear-every-frame with plain feedback (the common
    // case) instead of only the presets that actually meant to wipe.
    auto rootList = std::make_unique<EffectListEffect>();
    EffectListEffect* rootPtr = rootList.get();
    Reader r(data, len);
    r.skip(kMagicLen);
    parseListBody(r, len,
                  [rootPtr](std::unique_ptr<Effect> e) { rootPtr->addChild(std::move(e)); },
                  report, 0, rootPtr);
    if (rootList->childCount() == 0 && report.error.empty())
        report.error = "no supported components found";
    if (rootList->childCount() > 0) host.add(std::move(rootList));

    // Modifier/fragment presets (no content source) keep the previous image;
    // everything else starts deterministic from black. forceInherit (album
    // sequence playback) skips this and always inherits.
    if (forceInherit) {
        host.inheritCanvas = true;
    } else {
        host.inheritCanvas = host.count() > 0 && !stackRendersContent(host);
        if (!host.inheritCanvas) host.clearCanvas();
    }
    return report.ok();
}

bool loadAvsPresetFile(EffectHost& host, const std::string& path,
                       AvsLoadReport& report, bool forceInherit)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { report.error = "cannot open " + path; return false; }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf((size_t)std::max(0L, sz));
    size_t rd = buf.empty() ? 0 : std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    buf.resize(rd);

    size_t slash = path.find_last_of('/');
    report.presetName = (slash == std::string::npos) ? path : path.substr(slash + 1);
    setPresetResourceDir(slash == std::string::npos ? "." : path.substr(0, slash));
    return loadAvsPreset(host, buf.data(), buf.size(), report, forceInherit);
}

std::string AvsLoadReport::summary() const
{
    std::string s = presetName + ": ";
    if (!error.empty()) return s + "ERROR " + error;
    s += std::to_string(loaded.size()) + " full";
    if (!loaded.empty()) {
        s += ":";
        for (const auto& l : loaded) s += " [" + l + "]";
    }
    if (!partial.empty()) {
        s += ", partial:";
        for (const auto& p : partial) s += " [" + p + "]";
    }
    if (!skipped.empty()) {
        s += ", skipped:";
        for (const auto& k : skipped) s += " [" + k + "]";
    }
    return s;
}

} // namespace viz
