//
// DelayEffects.cpp — see DelayEffects.h.
//
#include "DelayEffects.h"
#include <algorithm>

namespace viz {

// History memory budget per queue. 8-bit RGB at 1280x800 is ~3MB/frame, so
// this allows ~80 frames at full internal resolution — plenty for the delays
// classic presets actually use.
static const size_t kQueueBudgetBytes = 256u << 20;

void FrameQueue::push(const Framebuffer& fb)
{
    if (fb.w != w || fb.h != h) { frames.clear(); w = fb.w; h = fb.h; }
    std::vector<uint8_t> enc((size_t)w * h * 3);
    const float* p = fb.px.data();
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i, p += 4) {
        enc[i * 3 + 0] = (uint8_t)(std::clamp(p[0], 0.f, 1.f) * 255.f + 0.5f);
        enc[i * 3 + 1] = (uint8_t)(std::clamp(p[1], 0.f, 1.f) * 255.f + 0.5f);
        enc[i * 3 + 2] = (uint8_t)(std::clamp(p[2], 0.f, 1.f) * 255.f + 0.5f);
    }
    frames.push_back(std::move(enc));
}

bool FrameQueue::readOldest(Framebuffer& fb, size_t depth)
{
    const size_t frameBytes = (size_t)w * h * 3;
    size_t maxFrames = frameBytes ? std::max<size_t>(1, kQueueBudgetBytes / frameBytes) : 1;
    depth = std::min(depth, maxFrames - 1);

    while (frames.size() > depth + 1) frames.pop_front();
    if (frames.size() < depth + 1) return false;

    const uint8_t* enc = frames.front().data();
    float* p = fb.px.data();
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i, p += 4) {
        p[0] = enc[i * 3 + 0] / 255.f;
        p[1] = enc[i * 3 + 1] / 255.f;
        p[2] = enc[i * 3 + 2] / 255.f;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Video Delay
// ---------------------------------------------------------------------------
void VideoDelayEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                              const VizFrame& a, const EffectContext& /*ctx*/)
{
    if (a.beat) { _framesPerBeat = std::max(1, _framesSinceBeat); _framesSinceBeat = 0; }
    else _framesSinceBeat++;

    size_t d = useBeats > 0.5f
             ? (size_t)std::min(400, _framesPerBeat * std::clamp((int)delay, 0, 16))
             : (size_t)std::clamp((int)delay, 0, 200);

    _q.push(cur);
    _q.readOldest(cur, d);
}

// ---------------------------------------------------------------------------
//  Multi Delay — 6 global buffers
// ---------------------------------------------------------------------------
static FrameQueue& multiDelayBuffer(int i)
{
    static FrameQueue pool[6];
    return pool[std::clamp(i, 0, 5)];
}

void resetDelayBuffers()
{
    for (int i = 0; i < 6; ++i) {
        FrameQueue& q = multiDelayBuffer(i);
        q.frames.clear();
        q.w = q.h = 0;
    }
}

void MultiDelayEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                              const VizFrame& a, const EffectContext& /*ctx*/)
{
    if (a.beat) { _framesPerBeat = std::max(1, _framesSinceBeat); _framesSinceBeat = 0; }
    else _framesSinceBeat++;

    FrameQueue& q = multiDelayBuffer((int)buffer);
    size_t d = useBeats > 0.5f
             ? (size_t)std::min(400, _framesPerBeat * std::clamp((int)delay, 0, 16))
             : (size_t)std::clamp((int)delay, 0, 200);

    switch ((int)mode) {
        case 1:                                    // input: feed the buffer
            q.push(cur);
            while (q.frames.size() > d + 1) q.frames.pop_front();
            break;
        case 2:                                    // output: emit delayed frame
            if (q.w == cur.w && q.h == cur.h)
                q.readOldest(cur, d);
            break;
        default: break;                            // disabled
    }
}

} // namespace viz
