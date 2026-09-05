//
// DelayEffects.h — frame-history delay effects, our takes on the Holden APEs
// Video Delay (e_videodelay.cpp) and Multi Delay (e_multidelay.cpp), BSD-3.
//
// Frames are stored 8-bit RGB (not float) and the history length is capped by
// a memory budget, so long delays degrade gracefully instead of eating RAM.
//
#pragma once
#include "Effect.h"
#include <cstdint>
#include <deque>
#include <vector>

namespace viz {

// A bounded frame-history ring: push the current frame, read the oldest.
struct FrameQueue {
    int w = 0, h = 0;
    std::deque<std::vector<uint8_t>> frames;   // 8-bit RGB, w*h*3 each

    void push(const Framebuffer& fb);          // resets on size change
    // Blit the oldest frame into fb if the queue holds > `depth` frames,
    // dropping history beyond `depth`. Returns false if not filled yet.
    bool readOldest(Framebuffer& fb, size_t depth);
};

// Drop all frames held in the 6 global Multi Delay buffers (preset reset).
void resetDelayBuffers();

// Holden04: Video Delay — output the frame from N frames (or N beats) ago.
class VideoDelayEffect : public Effect {
public:
    const char* name() const override { return "Video Delay"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float useBeats = 0.f;    // >0.5: delay is in beats (measured in frames)
    float delay    = 15.f;   // frames (0..200) or beats (0..16)

    std::vector<Param> params() override {
        return {{"use_beats", 0.f, 1.f, &useBeats},
                {"delay", 0.f, 200.f, &delay}};
    }

private:
    FrameQueue _q;
    int _framesSinceBeat = 0, _framesPerBeat = 30;
};

// Holden05: Multi Delay — 6 process-global delay buffers; "input" instances
// feed a buffer, "output" instances emit its delayed content further down the
// stack (or in another effect list entirely).
class MultiDelayEffect : public Effect {
public:
    const char* name() const override { return "Multi Delay"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float mode     = 1.f;    // 0 disabled, 1 input, 2 output
    float buffer   = 0.f;    // 0..5
    float useBeats = 0.f;
    float delay    = 8.f;    // frames (or beats)

    std::vector<Param> params() override {
        return {{"mode", 0.f, 2.f, &mode},
                {"buffer", 0.f, 5.f, &buffer},
                {"use_beats", 0.f, 1.f, &useBeats},
                {"delay", 0.f, 200.f, &delay}};
    }

private:
    int _framesSinceBeat = 0, _framesPerBeat = 30;
};

} // namespace viz
