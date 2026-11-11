//
// host_selftest.cpp — the effect host's preset transitions.
//
//   registers   a preset that draws only while reg00 is set keeps drawing
//               while it fades out (loading the next preset resets the
//               shared state; the outgoing preset keeps its own copy), and
//               the new preset starts from fresh registers
//   resize      a fade across a change of render size blends instead of
//               cutting (High resolution applies to some presets only)
//   two hosts   hosts rendering in turn each keep their own shared state
//               (registers, line mode): what a second host in the app, such
//               as preset thumbnails, needs
//
#include "testsupport.h"
#include "Presets.h"
#include "Superscope.h"
#include "UtilEffects.h"
#include "LineMode.h"
#include <cmath>
#include <cstdio>
#include <memory>

using namespace viz;

static int gFail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); ++gFail; } } while (0)

static std::unique_ptr<Effect> clearTo(float v)
{
    auto e = effectRegistry().at("clear_screen")();
    e->setParam("r", v); e->setParam("g", v); e->setParam("b", v);
    return e;
}

static double meanOf(const Framebuffer& fb, int y0 = 0, int y1 = -1)
{
    if (y1 < 0) y1 = fb.h;
    double s = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < fb.w; ++x) {
            const float* p = fb.at(x, y);
            s += (p[0] + p[1] + p[2]) / 3.0;
        }
    return s / ((double)fb.w * (y1 - y0));
}

static void render(EffectHost& host, int frames, int& n)
{
    VizFrame f;
    EffectContext ctx;
    for (int i = 0; i < frames; ++i, ++n) {
        test::synthAudio(f, n);
        ctx.frame = (uint64_t)n;
        ctx.time = n / 60.0;
        ctx.dt = 1.0 / 60.0;
        host.renderFrame(f, ctx);
    }
}

static void testRegisters()
{
    std::printf("registers\n");
    resetSharedState();
    EffectHost host;
    host.resize(320, 180);
    host.add(clearTo(0.f));
    // A horizontal line across the middle, drawn only while reg00 is 1 (set
    // once, in init, as presets that coordinate through registers do).
    auto ss = std::make_unique<SuperscopeEffect>();
    ss->setScripts("reg00=1;", "n=200;", "", "x=i*2-1; y=0; red=1; green=1; blue=1; skip=equal(reg00,0);");
    host.add(std::move(ss));
    int n = 0;
    render(host, 5, n);
    const int mid = 90;
    const double before = meanOf(host.currentSynced(), mid - 2, mid + 3);
    CHECK(before > 0.15, "the outgoing preset doesn't draw to begin with (%.3f)", before);

    // Load the next preset the way applyPreset does: crossfade, reset, new stack.
    host.beginCrossfade(1.0);
    resetSharedState();
    host.clearEffects();
    host.add(clearTo(0.f));
    CHECK(*eel::VM::globalRegSlot(0) == 0.0, "the new preset doesn't start with fresh registers");
    render(host, 18, n);                                 // 0.3 s in: mostly the outgoing preset
    const double during = meanOf(host.currentSynced(), mid - 2, mid + 3);
    std::printf("  middle rows: %.3f before, %.3f 0.3 s into the fade\n", before, during);
    CHECK(during > before * 0.5, "the outgoing preset stopped drawing during its fade (%.3f)", during);
    CHECK(*eel::VM::globalRegSlot(0) == 0.0, "the outgoing preset's registers leaked into the new one");
    render(host, 60, n);                                 // fade over
    CHECK(meanOf(host.currentSynced()) < 0.01, "the fade didn't finish on the new preset");
}

static void testResize()
{
    std::printf("resize\n");
    resetSharedState();
    EffectHost host;
    host.resize(160, 90);
    host.add(clearTo(1.f));                              // white
    int n = 0;
    render(host, 2, n);
    host.beginCrossfade(1.0);
    resetSharedState();
    host.clearEffects();
    host.add(clearTo(0.f));                              // black, at twice the size
    host.resize(320, 180);
    render(host, 6, n);                                  // 0.1 s in: almost all white
    const double m = meanOf(host.currentSynced());
    std::printf("  mean 0.1 s into the fade: %.3f\n", m);
    CHECK(m > 0.8, "a fade across a size change cut to the new preset (%.3f)", m);
}

// A stack that counts its frames in reg00 and sets a line mode.
static void counting(EffectHost& host, int lineWidth)
{
    host.clearEffects();
    host.add(clearTo(0.f));
    auto mode = std::make_unique<SetRenderModeEffect>();
    mode->lineWidth = (float)lineWidth;
    host.add(std::move(mode));
    auto ss = std::make_unique<SuperscopeEffect>();
    ss->setScripts("", "reg00=reg00+1;", "", "x=i*2-1; y=0;");
    host.add(std::move(ss));
}

static void testTwoHosts()
{
    std::printf("two hosts\n");
    EffectHost a, b;
    a.resize(64, 36); b.resize(64, 36);
    a.activate(); resetSharedState(); counting(a, 3);
    b.activate(); resetSharedState(); counting(b, 7);
    int na = 0, nb = 0;
    render(a, 10, na);
    render(b, 4, nb);
    render(a, 5, na);
    a.activate();
    const double ra = *eel::VM::globalRegSlot(0);
    const int wa = lineMode().width;
    b.activate();
    const double rb = *eel::VM::globalRegSlot(0);
    const int wb = lineMode().width;
    std::printf("  reg00: a %.0f (15 frames), b %.0f (4 frames); line width a %d, b %d\n", ra, rb, wa, wb);
    CHECK(ra == 15.0, "host a's registers were disturbed by host b (reg00 %.0f, want 15)", ra);
    CHECK(rb == 4.0, "host b's registers were disturbed by host a (reg00 %.0f, want 4)", rb);
    CHECK(wa == 3 && wb == 7, "line mode leaked between hosts (a %d, b %d)", wa, wb);
}

int main()
{
    testRegisters();
    testResize();
    testTwoHosts();
    if (gFail) { std::printf("host_selftest: %d FAILED\n", gFail); return 1; }
    std::printf("host_selftest: ALL PASS\n");
    return 0;
}
