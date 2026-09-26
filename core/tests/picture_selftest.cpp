//
// picture_selftest.cpp — the display-stage picture layer (Picture.h):
// built-in look tables, the .cube reader, and (with a GPU) the picture pass
// itself through gpu::renderPicture. Exit 0 on success.
//
//   picture_selftest [outdir]    — also writes a few PNGs to eyeball
//
#include "testsupport.h"
#include "Picture.h"
#include "GpuFx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace viz;

static int gFailures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { gFailures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static float luma(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

static void testLooks()
{
    const auto& names = lookNames();
    CHECK(names.size() >= 8 && names[0] == "Off", "look names start with Off (%zu names)", names.size());

    LookTable t;
    CHECK(buildLook(0, t) && t.empty(), "Off bakes to an empty table");
    CHECK(!buildLook(-1, t) && !buildLook((int)names.size(), t), "unknown look index rejected");

    uint64_t lastVersion = 0;
    for (int i = 1; i < (int)names.size(); ++i) {
        CHECK(buildLook(i, t), "build %s", names[i].c_str());
        CHECK(t.size == kLookSize && !t.empty(), "%s has a %d^3 table", names[i].c_str(), kLookSize);
        CHECK(t.version != lastVersion, "%s gets a fresh version", names[i].c_str());
        lastVersion = t.version;
        bool finite = true, inRange = true;
        for (float v : t.rgb) { finite &= std::isfinite(v); inRange &= v >= 0.f && v <= 1.f; }
        CHECK(finite && inRange, "%s: every entry finite and within 0..1", names[i].c_str());
        // Black stays dark, white stays bright, grey stays in between.
        float k[3] = {0, 0, 0}, w[3] = {1, 1, 1}, g[3] = {0.5f, 0.5f, 0.5f}, ko[3], wo[3], go[3];
        sampleLook(t, k, ko); sampleLook(t, w, wo); sampleLook(t, g, go);
        // Black is the screen off: every look keeps it black, except the
        // one whose whole point is lifted blacks.
        if (names[i] == "Faded film")
            CHECK(luma(ko) < 0.08f, "%s: black stays dark (%.3f)", names[i].c_str(), luma(ko));
        else
            CHECK(luma(ko) < 0.015f && std::max(ko[0], std::max(ko[1], ko[2])) < 0.025f,
                  "%s: black stays black (%.3f %.3f %.3f)", names[i].c_str(), ko[0], ko[1], ko[2]);
        CHECK(luma(wo) > 0.8f, "%s: white stays bright (%.3f)", names[i].c_str(), luma(wo));
        CHECK(luma(go) > luma(ko) && luma(go) < luma(wo), "%s: grey in between", names[i].c_str());
    }

    auto look = [&](const char* name, const float in[3], float out[3]) {
        for (int i = 0; i < (int)names.size(); ++i)
            if (names[i] == name) { LookTable lt; buildLook(i, lt); sampleLook(lt, in, out); return; }
        CHECK(false, "no look named %s", name);
    };
    float grey[3] = {0.5f, 0.5f, 0.5f}, o[3];
    look("Warm", grey, o);  CHECK(o[0] > o[2] + 0.1f, "Warm warms grey (r %.3f b %.3f)", o[0], o[2]);
    look("Cold", grey, o);  CHECK(o[2] > o[0] + 0.1f, "Cold cools grey (r %.3f b %.3f)", o[0], o[2]);
    // ...and visibly shifts the saturated blues visualizers are full of.
    float blue[3] = {0.2f, 0.4f, 0.9f}, bo[3];
    look("Warm", blue, bo);
    CHECK((bo[0] - bo[2]) > (blue[0] - blue[2]) + 0.15f, "Warm shifts blue toward amber (%.2f %.2f %.2f)", bo[0], bo[1], bo[2]);
    float red[3] = {0.9f, 0.1f, 0.1f};
    look("Noir", red, o);   CHECK(std::fabs(o[0] - o[1]) < 0.01f && std::fabs(o[1] - o[2]) < 0.01f, "Noir is monochrome");
    look("Sepia", red, o);  CHECK(o[0] >= o[1] && o[1] >= o[2], "Sepia is brown-toned (%.2f %.2f %.2f)", o[0], o[1], o[2]);
    float muted[3] = {0.6f, 0.45f, 0.4f}, mo[3];
    look("Vivid", muted, mo);
    CHECK((mo[0] - mo[2]) > (muted[0] - muted[2]), "Vivid increases saturation");
}

static void testCube()
{
    std::string err;
    LookTable t;
    // Identity 2^3 table with comments, a title and CRLF line ends.
    const char* identity =
        "# identity\r\nTITLE \"id\"\r\nLUT_3D_SIZE 2\r\n"
        "0 0 0\r\n1 0 0\r\n0 1 0\r\n1 1 0\r\n0 0 1\r\n1 0 1\r\n0 1 1\r\n1 1 1\r\n";
    CHECK(parseCube(identity, t, &err), "identity cube parses: %s", err.c_str());
    CHECK(t.size == kLookSize, "baked to %d^3", kLookSize);
    float in[3] = {0.25f, 0.6f, 0.9f}, out[3];
    sampleLook(t, in, out);
    CHECK(std::fabs(out[0] - in[0]) < 1e-3f && std::fabs(out[1] - in[1]) < 1e-3f && std::fabs(out[2] - in[2]) < 1e-3f,
          "identity cube is identity (%.3f %.3f %.3f)", out[0], out[1], out[2]);

    // Domain 0..2: our 0..1 input covers the lower half of the table.
    const char* domain = "LUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n"
        "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
    CHECK(parseCube(domain, t, &err), "domain cube parses: %s", err.c_str());
    float w[3] = {1, 1, 1};
    sampleLook(t, w, out);
    CHECK(std::fabs(out[0] - 0.5f) < 1e-3f, "DOMAIN_MAX 2 maps white to the table middle (%.3f)", out[0]);

    // A 1D table: invert.
    const char* inv1d = "LUT_1D_SIZE 2\n1 1 1\n0 0 0\n";
    CHECK(parseCube(inv1d, t, &err), "1D cube parses: %s", err.c_str());
    float q[3] = {0.2f, 0.5f, 0.8f};
    sampleLook(t, q, out);
    CHECK(std::fabs(out[0] - 0.8f) < 2e-3f && std::fabs(out[2] - 0.2f) < 2e-3f, "1D invert (%.3f %.3f)", out[0], out[2]);

    // Errors leave the table alone and say why.
    LookTable keep; buildLook(1, keep); uint64_t v = keep.version;
    CHECK(!parseCube("LUT_3D_SIZE 2\n0 0 0\n", keep, &err) && keep.version == v, "short data rejected");
    CHECK(err.find("expected") != std::string::npos, "short data: says how many rows (%s)", err.c_str());
    CHECK(!parseCube("0 0 0\n", keep, &err), "missing size rejected");
    CHECK(!parseCube("LUT_3D_SIZE 1\n", keep, &err), "size 1 rejected");
    CHECK(!parseCube("LUT_3D_SIZE 2\n0 0\n", keep, &err), "two-number row rejected");
    CHECK(!parseCube("LUT_3D_SIZE 2\nDOMAIN_MIN 1 1 1\nDOMAIN_MAX 0 0 0\n"
                     "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n", keep, &err), "inverted domain rejected");
    CHECK(!loadCubeFile("/nonexistent/x.cube", keep, &err), "missing file rejected");
}

// ---- GPU pass ------------------------------------------------------------
static Framebuffer frameOf(int W, int H, float r, float g, float b)
{
    Framebuffer fb; fb.resize(W, H); fb.clear(r, g, b, 1.f);
    return fb;
}

static double meanRegion(const std::vector<uint8_t>& px, int W, int x0, int y0, int x1, int y1, int ch)
{
    double s = 0; int n = 0;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) { s += px[((size_t)y * W + x) * 4 + ch]; n++; }
    return n ? s / n : 0;
}

static void writeRgba(const std::vector<uint8_t>& px, int W, int H, const std::string& path)
{
    Framebuffer fb; fb.resize(W, H);
    for (size_t i = 0; i < (size_t)W * H; ++i)
        for (int c = 0; c < 4; ++c) fb.px[i * 4 + c] = px[i * 4 + c] / 255.f;
    test::writePng(fb, path);
}

static void testGpu(const std::string& outDir)
{
    if (!gpu::available()) { printf(">> picture: no GPU, pass tests skipped\n"); return; }
    const int W = 160, H = 90;
    std::vector<uint8_t> out;

    // Off, 1:1: identity within the dither's ±1 LSB.
    Framebuffer ramp; ramp.resize(W, H);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        float* p = ramp.at(x, y); p[0] = x / (W - 1.f); p[1] = y / (H - 1.f); p[2] = 0.5f; p[3] = 1;
    }
    PictureSettings off;
    CHECK(gpu::renderPicture(ramp, off, W, H, out), "renderPicture runs");
    int maxErr = 0;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x)
        for (int c = 0; c < 3; ++c) {
            int want = (int)std::lround(std::min(1.f, std::max(0.f, ramp.at(x, y)[c])) * 255.f);
            maxErr = std::max(maxErr, std::abs(out[((size_t)y * W + x) * 4 + c] - want));
        }
    CHECK(maxErr <= 2, "Off at 1:1 is identity within dither (max error %d/255)", maxErr);

    // Upscaled 2x with sharp scaling: a white square on black keeps its
    // plateau, never overshoots, and the black stays black (no ringing halo).
    Framebuffer sq = frameOf(W, H, 0, 0, 0);
    for (int y = 30; y < 60; ++y) for (int x = 60; x < 100; ++x) { float* p = sq.at(x, y); p[0] = p[1] = p[2] = 1; }
    PictureSettings sharp; sharp.dither = false;
    CHECK(gpu::renderPicture(sq, sharp, W * 2, H * 2, out), "2x upscale runs");
    CHECK(meanRegion(out, W * 2, 140, 70, 180, 110, 1) > 250, "sharp: plateau stays white");
    CHECK(meanRegion(out, W * 2, 0, 0, 100, 40, 1) < 1, "sharp: black stays black");
    if (!outDir.empty()) writeRgba(out, W * 2, H * 2, outDir + "/picture_sharp.png");

    // Ringing shows on a mid-grey ground: plain Catmull-Rom dips below the
    // grey beside a white edge (a dark halo) and rises above it beside a
    // black one. The de-ringing clamp must keep both flat.
    for (float inner : {1.f, 0.f}) {
        Framebuffer ring = frameOf(W, H, 0.5f, 0.5f, 0.5f);
        for (int y = 30; y < 60; ++y) for (int x = 60; x < 100; ++x) { float* p = ring.at(x, y); p[0] = p[1] = p[2] = inner; }
        CHECK(gpu::renderPicture(ring, sharp, W * 2, H * 2, out), "ringing probe runs");
        int lo = 255, hi = 0;
        for (int y = 40; y < 140; ++y)                        // the grey beside the left and right edges
            for (int x : {100, 104, 108, 112, 116, 206, 210, 214, 218}) {
                int v = out[((size_t)y * W * 2 + x) * 4 + 1];
                lo = std::min(lo, v); hi = std::max(hi, v);
            }
        if (inner > 0.5f) CHECK(lo >= 126, "sharp: no dark halo beside a white edge (min %d)", lo);
        else              CHECK(hi <= 129, "sharp: no bright halo beside a black edge (max %d)", hi);
    }

    // Vignette darkens the corners of a flat grey frame, not the centre.
    Framebuffer grey = frameOf(W, H, 0.6f, 0.6f, 0.6f);
    PictureSettings vig; vig.vignette = 1.f; vig.dither = false;
    CHECK(gpu::renderPicture(grey, vig, W, H, out), "vignette runs");
    double centre = meanRegion(out, W, W / 2 - 8, H / 2 - 8, W / 2 + 8, H / 2 + 8, 1);
    double corner = meanRegion(out, W, 0, 0, 12, 12, 1);
    CHECK(corner < centre * 0.5 && centre > 140, "vignette: corners %.0f vs centre %.0f", corner, centre);

    // Grain: visible per-pixel variation, the mean kept.
    PictureSettings gr; gr.grain = 1.f; gr.dither = false;
    CHECK(gpu::renderPicture(grey, gr, W, H, out), "grain runs");
    double m = meanRegion(out, W, 0, 0, W, H, 1), var = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i) { double d = out[i * 4 + 1] - m; var += d * d; }
    double sd = std::sqrt(var / (W * H));
    CHECK(std::fabs(m - 153) < 4 && sd > 4, "grain: mean %.1f (want ~153), spread %.1f", m, sd);
    if (!outDir.empty()) writeRgba(out, W, H, outDir + "/picture_grain.png");

    // Glow brightens the neighbourhood of a bright point. At the app's real
    // frame size, where the pyramid gets its full depth (a tiny frame only
    // gets a few levels and a correspondingly short halo).
    {
        const int GW = 640, GH = 360;
        Framebuffer dot = frameOf(GW, GH, 0, 0, 0);
        for (int y = 174; y < 186; ++y) for (int x = 314; x < 326; ++x) { float* p = dot.at(x, y); p[0] = p[1] = p[2] = 1; }
        PictureSettings noGlow; noGlow.dither = false;
        PictureSettings glow = noGlow; glow.glow = 1.f;
        std::vector<uint8_t> a, b;
        CHECK(gpu::renderPicture(dot, noGlow, GW, GH, a) && gpu::renderPicture(dot, glow, GW, GH, b), "glow runs");
        double nearA = meanRegion(a, GW, 330, 174, 340, 186, 1), nearB = meanRegion(b, GW, 330, 174, 340, 186, 1);
        double farB = meanRegion(b, GW, 360, 174, 370, 186, 1);
        CHECK(nearB > nearA + 12, "glow: halo next to the point (%.1f vs %.1f)", nearB, nearA);
        CHECK(farB > 1 && farB < nearB, "glow: halo falls off with distance (%.1f then %.1f)", nearB, farB);
        CHECK(meanRegion(b, GW, 0, 0, 40, 40, 1) < 2, "glow: far corner stays dark");
        CHECK(meanRegion(b, GW, 316, 176, 324, 184, 1) > 250, "glow: the point itself stays white");
        if (!outDir.empty()) writeRgba(b, GW, GH, outDir + "/picture_glow.png");
    }

    // A look reaches the screen: Warm on grey, and strength 0 is a no-op.
    LookTable warm; buildLook(1, warm);
    PictureSettings lk; lk.look = &warm; lk.dither = false;
    CHECK(gpu::renderPicture(grey, lk, W, H, out), "look runs");
    double r = meanRegion(out, W, 0, 0, W, H, 0), bl = meanRegion(out, W, 0, 0, W, H, 2);
    CHECK(r > bl + 8, "Warm look on grey: r %.0f > b %.0f", r, bl);
    lk.lookStrength = 0.f;
    CHECK(gpu::renderPicture(grey, lk, W, H, out), "look strength 0 runs");
    CHECK(std::fabs(meanRegion(out, W, 0, 0, W, H, 0) - meanRegion(out, W, 0, 0, W, H, 2)) < 1, "strength 0 leaves grey");

    // The pass never writes the frame it reads (presets feed that frame forward).
    Framebuffer before = grey;
    PictureSettings all; all.look = &warm; all.grain = 1; all.vignette = 1; all.glow = 1; all.scanlines = 1;
    CHECK(gpu::renderPicture(grey, all, W * 2, H * 2, out), "everything on runs");
    gpu::flush();
    CHECK(grey.px == before.px, "the source frame is left untouched");
    if (!outDir.empty()) writeRgba(out, W * 2, H * 2, outDir + "/picture_all.png");
}

int main(int argc, const char** argv)
{
    std::string outDir = argc > 1 ? argv[1] : "";
    testLooks();
    testCube();
    testGpu(outDir);
    printf(">> picture: %s (%d failures)\n", gFailures ? "FAIL" : "ok", gFailures);
    return gFailures ? 1 : 0;
}
