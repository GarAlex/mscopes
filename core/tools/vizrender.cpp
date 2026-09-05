//
// vizrender — render a preset to a PNG frame sequence, headless, anywhere.
//
//   vizrender <preset.avs|.json> [--audio track.wav] [--frames N] [--fps 60]
//             [--size WxH] [--out dir]
//   vizrender --builtin "<name>"    (or --list to see the built-in presets)
//
// Without --audio it uses the test suite's synthetic signal. With a WAV
// (PCM 16-bit or float32, any channel count) the real Analyzer + beat
// detector run over it at the render frame rate. Frames land in
// <out>/frame_NNNNN.png; make a clip with e.g.
//   ffmpeg -framerate 60 -i out/frame_%05d.png -i track.wav -shortest clip.mp4
//
#include "../tests/testsupport.h"
#include "Analyzer.h"
#include "AvsPreset.h"
#include "JsonPreset.h"
#include "Presets.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace viz;

struct Wav { int rate = 0, channels = 0; std::vector<float> samples; };  // interleaved

static bool readWav(const std::string& path, Wav& w)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<unsigned char> d;
    unsigned char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return false;
    auto u16 = [&](size_t o) { return (unsigned)d[o] | ((unsigned)d[o + 1] << 8); };
    auto u32 = [&](size_t o) { return u16(o) | (u16(o + 2) << 16); };
    int fmt = 0, bits = 0;
    size_t pos = 12;
    while (pos + 8 <= d.size()) {
        size_t sz = u32(pos + 4), body = pos + 8;
        if (!std::memcmp(d.data() + pos, "fmt ", 4)) {
            fmt = (int)u16(body); w.channels = (int)u16(body + 2);
            w.rate = (int)u32(body + 4); bits = (int)u16(body + 14);
        } else if (!std::memcmp(d.data() + pos, "data", 4)) {
            size_t end = std::min(d.size(), body + sz);
            if (fmt == 1 && bits == 16) {
                for (size_t o = body; o + 1 < end; o += 2)
                    w.samples.push_back((float)(short)u16(o) / 32768.f);
            } else if (fmt == 3 && bits == 32) {
                for (size_t o = body; o + 3 < end; o += 4) {
                    float v; std::memcpy(&v, d.data() + o, 4); w.samples.push_back(v);
                }
            } else {
                fprintf(stderr, "vizrender: unsupported WAV format %d/%d-bit\n", fmt, bits);
                return false;
            }
        }
        pos = body + sz + (sz & 1);
    }
    return w.rate > 0 && w.channels > 0 && !w.samples.empty();
}

int main(int argc, const char** argv)
{
    std::string preset, audio, out = "out", builtin;
    int frames = 300, fps = 60, W = 640, H = 360;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--list") {
            for (const auto& p : builtinPresets()) printf("%s\n", p.name.c_str());
            return 0;
        }
        else if (a == "--builtin") builtin = next();
        else if (a == "--audio") audio = next();
        else if (a == "--frames") frames = atoi(next());
        else if (a == "--fps") fps = std::max(1, atoi(next()));
        else if (a == "--size") { const char* s = next(); sscanf(s, "%dx%d", &W, &H); }
        else if (a == "--out") out = next();
        else if (a[0] == '-') { fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
        else preset = a;
    }
    if (preset.empty() && builtin.empty()) {
        fprintf(stderr, "usage: vizrender <preset.avs|.json> | --builtin <name> | --list  [--audio t.wav] [--frames N] [--fps 60] [--size WxH] [--out dir]\n");
        return 1;
    }

    EffectHost host;
    host.resize(W, H);
    std::string err;
    bool ok = false;
    if (!builtin.empty()) {
        for (const auto& p : builtinPresets())
            if (p.name == builtin) { applyPreset(host, p); ok = true; }
        if (!ok) err = "no built-in preset named \"" + builtin + "\" (see --list)";
        preset = builtin;
    } else if (preset.size() > 5 && preset.substr(preset.size() - 5) == ".json") {
        ok = loadJsonPresetFile(host, preset, &err);
    } else {
        AvsLoadReport rep;
        ok = loadAvsPresetFile(host, preset, rep);
        err = rep.summary();
        fprintf(stderr, "%s\n", err.c_str());
    }
    if (!ok) { fprintf(stderr, "vizrender: cannot load %s: %s\n", preset.c_str(), err.c_str()); return 2; }

    Wav wav;
    Analyzer analyzer;
    bool haveAudio = !audio.empty();
    if (haveAudio && !readWav(audio, wav)) { fprintf(stderr, "vizrender: cannot read %s\n", audio.c_str()); return 2; }
    if (haveAudio) {
        long total = (long)wav.samples.size() / wav.channels;
        frames = std::min<long>(frames, (total * fps) / wav.rate);
        fprintf(stderr, "audio: %d Hz, %d ch, %.1f s → %d frames at %d fps\n",
                wav.rate, wav.channels, (double)total / wav.rate, frames, fps);
    }

    VizFrame f;
    EffectContext ctx;
    ctx.dt = 1.0 / fps;
    size_t cursor = 0;
    for (int i = 0; i < frames; ++i) {
        if (haveAudio) {
            size_t hop = (size_t)wav.rate / fps;
            size_t avail = std::min(hop, wav.samples.size() / wav.channels - cursor);
            analyzer.push(wav.samples.data() + cursor * wav.channels, (int)avail, wav.channels);
            cursor += avail;
            analyzer.analyze(f, 1.0 / fps);
        } else {
            test::synthAudio(f, i);
        }
        ctx.frame = i; ctx.time = i / (double)fps;
        host.renderFrame(f, ctx);
        char name[64];
        snprintf(name, sizeof name, "/frame_%05d.png", i);
        if (!test::writePng(host.currentSynced(), out + name)) {
            fprintf(stderr, "vizrender: cannot write %s%s (does the directory exist?)\n", out.c_str(), name);
            return 3;
        }
    }
    fprintf(stderr, "wrote %d frames to %s/\n", frames, out.c_str());
    return 0;
}
