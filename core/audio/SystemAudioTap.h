//
// SystemAudioTap.h — capture the whole system audio output mix via a
// CoreAudio process tap + private aggregate device (macOS 14.4+).
//
// This is the standalone app's AudioSource: it grabs the digital output of
// EVERY app (Apple Music, Spotify, browser, system sounds), volume-independent
// and working on headphones — no microphone, no screen-recording permission.
//
#pragma once
#include <functional>
#include <string>
#include <CoreAudio/CoreAudio.h>

namespace viz {

class SystemAudioTap {
public:
    // Called from a real-time audio thread: interleaved float frames.
    using Callback = std::function<void(const float* interleaved,
                                        int numFrames, int channels,
                                        double sampleRate)>;

    explicit SystemAudioTap(Callback cb) : _cb(std::move(cb)) {}
    ~SystemAudioTap();

    // Returns true on success; on failure sets `err`.
    bool start(std::string& err);
    void stop();

    double sampleRate() const { return _sampleRate; }
    int    channels()   const { return _channels; }

private:
    Callback           _cb;
    AudioObjectID      _tapID   = kAudioObjectUnknown;
    AudioObjectID      _aggID   = kAudioObjectUnknown;
    AudioDeviceIOProcID _procID = nullptr;
    double             _sampleRate = 0;
    int                _channels   = 0;
    bool               _running    = false;
};

} // namespace viz
