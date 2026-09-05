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
#include <vector>
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

    // Test aid: stop the IO proc but keep the objects, so the engine's
    // stall watchdog sees callbacks cease — exactly what a device change or
    // a sleep/wake does to a live tap. (SIGUSR2 in the app triggers it.)
    void pauseForTest();

    double sampleRate() const { return _sampleRate; }
    int    channels()   const { return _channels; }
    AudioObjectID aggregateID() const { return _aggID; }
    // Number of process objects this tap mixes (0 = global tap mode).
    int processCount() const { return _processCount; }
    // The process objects this tap was built from (sorted; empty in global mode).
    const std::vector<AudioObjectID>& tappedProcesses() const { return _procs; }

    // The process objects currently producing output, minus this process
    // (sorted) — what a fresh tap would be built from.
    static std::vector<AudioObjectID> runningOutputProcesses();

    // Call `handler` (on the main queue) whenever the audio process list or
    // any process's running-output state changes. Installing twice replaces
    // the handler; an empty handler stops the watching.
    static void watchProcesses(std::function<void()> handler);

private:
    Callback           _cb;
    AudioObjectID      _tapID   = kAudioObjectUnknown;
    AudioObjectID      _aggID   = kAudioObjectUnknown;
    AudioDeviceIOProcID _procID = nullptr;
    double             _sampleRate = 0;
    int                _channels   = 0;
    bool               _running    = false;
    int                _processCount = 0;
    std::vector<AudioObjectID> _procs;
};

} // namespace viz
