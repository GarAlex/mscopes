//
// draw_selftest.cpp — the line and dot rasterizers (LineMode.h).
//
//   Authentic must be exactly the AVS rasterizer the classic effects always
//   used; Smooth must carry the same light as the hard line it replaces, at
//   any angle and sub-pixel offset, place things at sub-pixel precision,
//   and not bead at polyline joints. Then every drawing effect renders in
//   both modes without NaNs. Exit 0 on success.
//
//   draw_selftest [outdir]    — also writes smooth renders to eyeball
//
#include "testsupport.h"
#include "LineMode.h"
#include "Presets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace viz;

static int gFailures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { gFailures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static Framebuffer blank(int W, int H) { Framebuffer fb; fb.resize(W, H); fb.clear(0, 0, 0, 1); return fb; }

static double sumG(const Framebuffer& fb)
{
    double s = 0;
    for (size_t i = 0; i < fb.px.size(); i += 4) s += fb.px[i + 1];
    return s;
}

static bool allFinite(const Framebuffer& fb)
{
    for (float v : fb.px) if (!std::isfinite(v)) return false;
    return true;
}

struct Rng { uint32_t s = 12345; float f() { s = s * 1664525u + 1013904223u; return (s >> 8) * (1.f / 16777216.f); } };

static void setQuality(bool smooth, float widthScale = 1.f)
{
    drawQuality().smooth = smooth;
    drawQuality().widthScale = widthScale;
}

// Authentic: drawLineQ / putDotQ are the old integer calls on truncated
// positions, bit for bit, in every blend mode and width.
static void testAuthenticIsExact()
{
    setQuality(false);
    Rng rng;
    int mismatches = 0;
    for (int mode = 0; mode <= 9; ++mode)
        for (int width : {0, 1, 3}) {
            lineMode().blend = mode; lineMode().alpha = 0.6f; lineMode().width = 2;
            Framebuffer a = blank(96, 64), b = blank(96, 64);
            for (int k = 0; k < 40; ++k) {
                float x0 = rng.f() * 120 - 12, y0 = rng.f() * 90 - 12, x1 = rng.f() * 120 - 12, y1 = rng.f() * 90 - 12;
                float r = rng.f(), g = rng.f(), bl = rng.f();
                drawLineQ(a, x0, y0, x1, y1, r, g, bl, width);
                drawLineMode(b, (int)x0, (int)y0, (int)x1, (int)y1, r, g, bl, width);
                float px = rng.f() * 110 - 5, py = rng.f() * 80 - 5;
                putDotQ(a, px, py, r, g, bl);
                putLinePixel(b, (int)px, (int)py, r, g, bl);
            }
            if (a.px != b.px) mismatches++;
        }
    CHECK(mismatches == 0, "Authentic differs from the integer rasterizer in %d mode/width combinations", mismatches);
    lineMode() = LineMode();
}

// Smooth: a line of width w carries w of light per unit length, whatever
// its angle or offset; its centre sits where it was asked to.
static void testSmoothEnergy()
{
    setQuality(true);
    lineMode() = LineMode();
    lineMode().blend = 1;                         // additive on black: the value is the coverage
    for (int width : {1, 3}) {
        for (float deg : {0.f, 10.f, 22.5f, 30.f, 45.f, 60.f, 80.f, 90.f}) {
            const float L = 60.f, rad = deg * 3.14159265f / 180.f;
            const float cx = 64.3f, cy = 64.7f;
            float x0 = cx - std::cos(rad) * L * 0.5f, y0 = cy - std::sin(rad) * L * 0.5f;
            float x1 = cx + std::cos(rad) * L * 0.5f, y1 = cy + std::sin(rad) * L * 0.5f;
            Framebuffer fb = blank(128, 128);
            drawLineQ(fb, x0, y0, x1, y1, 1, 1, 1, width);
            // body light plus the two round half-width caps
            double want = L * width + 3.14159265 * (width * 0.5) * (width * 0.5);
            double got = sumG(fb);
            CHECK(std::fabs(got - want) / want < 0.06, "width %d at %.1f°: light %.1f, want %.1f", width, deg, got, want);
        }
    }
    // Column energy of a horizontal line equals its width at any offset.
    for (float off : {0.0f, 0.25f, 0.5f, 0.8f}) {
        Framebuffer fb = blank(64, 32);
        drawLineQ(fb, 5.f, 16.f + off, 60.f, 16.f + off, 1, 1, 1, 1);
        double col = 0, centroid = 0;
        for (int y = 0; y < 32; ++y) { double v = fb.at(30, y)[1]; col += v; centroid += v * (y + 0.5); }
        centroid /= std::max(col, 1e-9);
        CHECK(std::fabs(col - 1.0) < 1e-3, "horizontal line at +%.2f: column light %.4f", off, col);
        CHECK(std::fabs(centroid - (16.0 + off)) < 0.02, "horizontal line at +%.2f: centre %.3f", off, centroid);
    }
    lineMode() = LineMode();
}

// Polylines: a straight or gently curving run of short segments carries the
// same light as one line (no beads at the joints, even additive); capping
// every segment instead would bead. Sharp corners get a round join that
// stays close to line brightness.
static void testJoints()
{
    setQuality(true);
    lineMode() = LineMode();
    lineMode().blend = 1;
    auto polyline = [](Framebuffer& fb, const std::vector<std::pair<float, float>>& pts, bool joined) {
        for (size_t i = 1; i < pts.size(); ++i) {
            bool cs = true, ce = true;
            if (joined) {
                cs = i == 1 || sharpTurn(pts[i - 2].first, pts[i - 2].second, pts[i - 1].first,
                                         pts[i - 1].second, pts[i].first, pts[i].second);
                ce = i == pts.size() - 1;
            }
            drawLineQ(fb, pts[i - 1].first, pts[i - 1].second, pts[i].first, pts[i].second,
                      0.4f, 0.4f, 0.4f, 1, cs, ce);
        }
    };
    // A diagonal run cut into 3-px segments vs the same line drawn once.
    std::vector<std::pair<float, float>> run;
    for (int i = 0; i <= 30; ++i) run.push_back({10.3f + i * 3.f * 0.8f, 20.6f + i * 3.f * 0.6f});
    Framebuffer one = blank(128, 96), joined = blank(128, 96), capped = blank(128, 96);
    drawLineQ(one, run.front().first, run.front().second, run.back().first, run.back().second, 0.4f, 0.4f, 0.4f, 1);
    polyline(joined, run, true);
    polyline(capped, run, false);
    float dj = 0, dc = 0;
    for (size_t i = 1; i < one.px.size(); i += 4) {
        dj = std::max(dj, std::fabs(joined.px[i] - one.px[i]));
        dc = std::max(dc, std::fabs(capped.px[i] - one.px[i]));
    }
    CHECK(dj < 0.03f, "segmented straight run equals one line (max difference %.3f)", dj);
    CHECK(dc > 0.1f, "capping every segment would bead (max difference %.3f)", dc);

    // A gentle curve: joined segments stay near line brightness everywhere.
    std::vector<std::pair<float, float>> arc;
    for (int i = 0; i <= 40; ++i) { float a = i * 0.035f; arc.push_back({64 + std::cos(a) * 50, 90 - std::sin(a) * 50}); }
    Framebuffer curve = blank(128, 96);
    polyline(curve, arc, true);
    float peak = 0;
    for (size_t i = 1; i < curve.px.size(); i += 4) peak = std::max(peak, curve.px[i]);
    CHECK(peak <= 0.4f * 1.15f, "gentle curve: no beads (peak %.3f vs 0.4)", peak);

    // Sharp zigzag: round joins, peaks within a third of line brightness.
    std::vector<std::pair<float, float>> zig;
    for (int i = 0; i <= 15; ++i) zig.push_back({5 + i * 8.f, 48 + ((i & 1) ? 12.f : -12.f)});
    Framebuffer zz = blank(140, 96);
    polyline(zz, zig, true);
    peak = 0;
    for (size_t i = 1; i < zz.px.size(); i += 4) peak = std::max(peak, zz.px[i]);
    CHECK(peak <= 0.4f * 1.35f, "sharp corners: round joins stay close to line brightness (peak %.3f)", peak);
    lineMode() = LineMode();
}

// Dots: one pixel of light at any sub-pixel position, centred on it; a wider
// widthScale makes a disc.
static void testDots()
{
    setQuality(true);
    lineMode() = LineMode();
    lineMode().blend = 1;
    for (float fx : {10.0f, 10.3f, 10.5f, 10.9f})
        for (float fy : {7.0f, 7.25f, 7.75f}) {
            Framebuffer fb = blank(24, 16);
            putDotQ(fb, fx, fy, 1, 1, 1);
            double s = 0, cx = 0, cy = 0;
            for (int y = 0; y < 16; ++y) for (int x = 0; x < 24; ++x) {
                double v = fb.at(x, y)[1]; s += v; cx += v * (x + 0.5); cy += v * (y + 0.5);
            }
            CHECK(std::fabs(s - 1.0) < 1e-4, "dot at (%.2f, %.2f): light %.4f", fx, fy, s);
            CHECK(std::fabs(cx / s - fx) < 1e-3 && std::fabs(cy / s - fy) < 1e-3,
                  "dot at (%.2f, %.2f): centre (%.3f, %.3f)", fx, fy, cx / s, cy / s);
        }
    setQuality(true, 2.f);
    Framebuffer fb = blank(24, 16);
    putDotQ(fb, 12.2f, 8.4f, 1, 1, 1);
    double s = sumG(fb);
    CHECK(s > 2.5 && s < 4.0, "widthScale 2 makes a disc of about pi (%.2f)", s);
    // Lines scale with widthScale too.
    Framebuffer ln = blank(64, 32);
    drawLineQ(ln, 5.f, 16.3f, 60.f, 16.3f, 1, 1, 1, 1);
    double col = 0;
    for (int y = 0; y < 32; ++y) col += ln.at(30, y)[1];
    CHECK(std::fabs(col - 2.0) < 1e-3, "widthScale 2: a 1-px line is 2 px wide (%.3f)", col);
    lineMode() = LineMode();
    setQuality(true);
}

// Nothing unreasonable input can do: off-screen, huge, NaN, zero length.
static void testRobustness()
{
    setQuality(true);
    Framebuffer fb = blank(32, 32);
    const float nan = std::nanf("");
    drawLineQ(fb, -1e9f, -1e9f, 1e9f, 1e9f, 1, 1, 1, 255);
    drawLineQ(fb, nan, 3, 4, 5, 1, 1, 1);
    drawLineQ(fb, 5, 5, 5, 5, 1, 1, 1);
    drawLineQ(fb, -100, 10, -50, 20, 1, 1, 1);
    putDotQ(fb, nan, nan, 1, 1, 1);
    putDotQ(fb, -0.4f, 31.9f, 1, 1, 1);
    CHECK(allFinite(fb), "robust against NaN, huge and off-screen coordinates");
    setQuality(false);
    Framebuffer fa = blank(32, 32);
    drawLineQ(fa, nan, 3, 4, 5, 1, 1, 1);
    putDotQ(fa, nan, 2, 1, 1, 1);
    drawLineQ(fa, -1e9f, 5, 1e9f, 6, 1, 1, 1);
    CHECK(allFinite(fa), "Authentic path robust as well");
}

// Every drawing effect in both modes: finite, draws something, and the
// smooth mode actually takes a different path where it has one.
static void testEffects(const std::string& outDir)
{
    const char* keys[] = { "scope", "starfield", "spectrum_bars", "particles", "simple_scope", "ring",
                           "osc_star", "rot_star", "bass_spin", "dot_grid", "dot_plane", "dot_fountain",
                           "moving_particle", "superscope" };
    for (const char* key : keys) {
        Framebuffer out[2];
        for (int smooth = 0; smooth < 2; ++smooth) {
            setQuality(smooth == 1);
            EffectHost host;
            host.resize(320, 180);
            {
                auto fade = effectRegistry().at("feedback_warp")();
                fade->setParam("zoom", 1.f); fade->setParam("spin", 0.f); fade->setParam("decay", 0.9f);
                host.add(std::move(fade));
            }
            auto fx = effectRegistry().at(key)();
            if (std::string(key) == "bass_spin") fx->setParam("mode", 0.f);   // lines, not filled triangles
            host.add(std::move(fx));
            VizFrame f;
            EffectContext ctx;
            for (int i = 0; i < 90; ++i) {
                test::synthAudio(f, i);
                ctx.frame = i; ctx.time = i / 60.0;
                host.renderFrame(f, ctx);
            }
            out[smooth] = host.currentSynced();
            if (smooth && !outDir.empty()) test::writePng(out[smooth], outDir + "/smooth_" + key + ".png");
        }
        CHECK(allFinite(out[0]) && allFinite(out[1]), "%s: finite in both modes", key);
        CHECK(sumG(out[1]) > 1.0, "%s: smooth mode draws something", key);
        bool particlesAlwaysSmooth = std::string(key) == "particles";   // sub-pixel in every mode
        if (!particlesAlwaysSmooth)
            CHECK(out[0].px != out[1].px, "%s: smooth mode takes its own path", key);
    }
    setQuality(false);
}

int main(int argc, const char** argv)
{
    std::string outDir = argc > 1 ? argv[1] : "";
    testAuthenticIsExact();
    testSmoothEnergy();
    testJoints();
    testDots();
    testRobustness();
    testEffects(outDir);
    printf(">> draw: %s (%d failures)\n", gFailures ? "FAIL" : "ok", gFailures);
    return gFailures ? 1 : 0;
}
