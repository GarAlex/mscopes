//
// AudioCapture.mm — see AudioCapture.h.
//
#import "AudioCapture.h"
#import <AppKit/AppKit.h>
#import "Analyzer.h"
#import "SystemAudioTap.h"
#include "EngineLog.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

// One tap and the analyzer slot it feeds.
struct TapSlot {
    std::unique_ptr<viz::SystemAudioTap> tap;
    AudioObjectID proc = kAudioObjectUnknown;
    std::string name;                          // bundle id, kept for the log after the process is gone
    int index = 0;                             // analyzer slot
    std::atomic<float> peak{0};                // raw level, decays
    std::atomic<bool> hadSignal{false};        // produced audio since (re)open
    CFTimeInterval lastRestart = 0;
    int quietChecks = 0;
    int blindRetries = 0;                      // re-opens of a tap that never had signal
};

std::string procName(AudioObjectID proc)
{
    CFStringRef bid = nullptr; UInt32 sz = sizeof(bid);
    AudioObjectPropertyAddress ba = { kAudioProcessPropertyBundleID,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    std::string name;
    if (AudioObjectGetPropertyData(proc, &ba, 0, nullptr, &sz, &bid) == noErr && bid) {
        name = [(__bridge NSString*)bid UTF8String] ?: "";
        CFRelease(bid);
    }
    if (name.empty()) {
        pid_t pid = 0; sz = sizeof(pid);
        AudioObjectPropertyAddress pa = { kAudioProcessPropertyPID,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioObjectGetPropertyData(proc, &pa, 0, nullptr, &sz, &pid);
        name = "pid " + std::to_string(pid);
    }
    return name;
}

bool processRunningOutput(AudioObjectID proc)
{
    UInt32 running = 0, sz = sizeof(running);
    AudioObjectPropertyAddress ra = { kAudioProcessPropertyIsRunningOutput,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    return AudioObjectGetPropertyData(proc, &ra, 0, nullptr, &sz, &running) == noErr && running;
}

} // namespace

@implementation WVAudioCapture {
    viz::Analyzer*        _analyzer;
    // Declared before the slots: the taps' callbacks count into it, and C++
    // members are destroyed in reverse order (the slots go first).
    std::atomic<unsigned long long> _cbs;
    std::vector<std::unique_ptr<TapSlot>> _slots;
    // Stall watchdog (see -watchdog:).
    unsigned long long    _cbsSeen;
    CFTimeInterval        _cbsSeenAt;
    CFTimeInterval        _lastTapRestart;
    // Listeners.
    BOOL                  _listenersInstalled;
    id                    _wakeObserver;
    AudioObjectPropertyListenerBlock _defaultDeviceListener;
    unsigned              _procGen;        // coalesces bursts of process events
    double                _lastOpenMs;     // how long the last tap took to open (main thread)
}

- (instancetype)initWithAnalyzer:(viz::Analyzer *)analyzer
{
    if ((self = [super init])) {
        _analyzer = analyzer;
        _cbs = 0;
    }
    return self;
}

- (void)dealloc { [self stop]; }

- (unsigned long long)callbacks { return _cbs.load(); }
- (NSUInteger)tapCount { return _slots.size(); }

- (float)inputPeak
{
    float m = 0.f;
    for (auto& sl : _slots) m = std::max(m, sl->peak.load());
    return m;
}

- (BOOL)start:(std::string &)err
{
    if (_capturing) return YES;
    _sampleRate = 0; _channels = 0;
    NSInteger taps = [self reconcileTapsBecause:"capture start"];
    if (taps == 0) {
        // Nothing is playing yet; taps open as soon as something does. Still
        // make sure a tap CAN be made (the system prompts for permission on
        // the first one): try a throwaway global tap and surface its error.
        viz::SystemAudioTap probe([](const float*, int, int, double) {});
        std::vector<AudioObjectID> none;
        if (!probe.start(err, none)) return NO;
        _sampleRate = probe.sampleRate(); _channels = probe.channels();
        _analyzer->setSampleRate(_sampleRate);
        probe.stop();
    }
    _capturing = YES;
    _tapRestarts = 0;
    [self installListeners];
    return YES;
}

- (void)stop
{
    [self removeListeners];
    _slots.clear();
    _capturing = NO;
}

- (int)freeSlotIndex
{
    for (int i = 0; i < viz::Analyzer::kSlots; ++i) {
        bool used = false;
        for (auto& sl : _slots) if (sl->index == i) { used = true; break; }
        if (!used) return i;
    }
    return -1;
}

/// (Re)open one slot's tap on its process.
- (BOOL)openSlot:(TapSlot*)slot err:(std::string&)err
{
    slot->tap.reset();
    viz::Analyzer* an = _analyzer;
    std::atomic<unsigned long long>* cbs = &_cbs;
    TapSlot* sp = slot;
    int idx = slot->index;
    auto tap = std::make_unique<viz::SystemAudioTap>(
        [an, cbs, sp, idx](const float* interleaved, int frames, int channels, double sr) {
            (void)sr;
            an->push(idx, interleaved, frames, channels);
            float m = 0.f;
            for (int i = 0, n = frames * channels; i < n; ++i) m = std::max(m, std::fabs(interleaved[i]));
            sp->peak.store(std::max(m, sp->peak.load() * 0.9f));
            if (m > 0.003f) sp->hadSignal.store(true, std::memory_order_relaxed);
            cbs->fetch_add(1);
        });
    std::vector<AudioObjectID> procs;
    if (slot->proc != kAudioObjectUnknown) procs.push_back(slot->proc);
    const CFTimeInterval t0 = CACurrentMediaTime();
    const bool started = tap->start(err, procs);
    _lastOpenMs = (CACurrentMediaTime() - t0) * 1000.0;
    if (!started) return NO;
    slot->tap = std::move(tap);
    slot->lastRestart = CACurrentMediaTime();
    slot->quietChecks = 0;
    slot->peak.store(0.f);
    slot->hadSignal.store(false);
    if (_sampleRate <= 0) {
        _sampleRate = slot->tap->sampleRate(); _channels = slot->tap->channels();
        _analyzer->setSampleRate(_sampleRate);
    }
    return YES;
}

/// Bring the taps in line with the processes producing output: one tap per
/// process, taps of processes that stopped are closed, healthy ones are
/// left alone. Returns the number of taps now open.
- (NSInteger)reconcileTapsBecause:(const char*)why
{
    auto wanted = viz::SystemAudioTap::runningOutputProcesses();
    if (wanted.size() > (size_t)viz::Analyzer::kSlots) wanted.resize(viz::Analyzer::kSlots);
    for (auto it = _slots.begin(); it != _slots.end();) {
        if (std::find(wanted.begin(), wanted.end(), (*it)->proc) == wanted.end()) {
            const std::string name = (*it)->name;
            const CFTimeInterval t0 = CACurrentMediaTime();
            it = _slots.erase(it);
            wvLog("tap closed: %s stopped playing (%s), %.0f ms", name.c_str(), why,
                  (CACurrentMediaTime() - t0) * 1000.0);
        } else ++it;
    }
    std::string err;
    for (AudioObjectID p : wanted) {
        bool have = false;
        for (auto& sl : _slots) if (sl->proc == p) { have = true; break; }
        if (have) continue;
        auto sl = std::make_unique<TapSlot>();
        sl->proc = p;
        sl->name = procName(p);
        sl->index = [self freeSlotIndex];
        if (sl->index < 0) break;
        if ([self openSlot:sl.get() err:err]) {
            wvLog("tap opened: %s (%s) -> slot %d, %.0f Hz, %d ch, %.0f ms", sl->name.c_str(), why,
                  sl->index, sl->tap->sampleRate(), sl->tap->channels(), _lastOpenMs);
            _slots.push_back(std::move(sl));
        } else {
            wvLog("tap open FAILED for %s: %s", sl->name.c_str(), err.c_str());
        }
    }
    _cbsSeen   = _cbs.load();
    _cbsSeenAt = CACurrentMediaTime();
    return (NSInteger)_slots.size();
}

- (void)stallForTest
{
    for (auto& sl : _slots) if (sl->tap) sl->tap->pauseForTest();
    wvLog("SIGUSR2: tap IO stopped on %zu tap(s) for the watchdog test", _slots.size());
    fprintf(stderr, "[VizEngine] SIGUSR2: tap IO stopped for the watchdog test\n");
}

/// Re-open every tap (device change, wake, all callbacks stopped). A
/// failure is logged and left to the watchdog.
- (void)restartTapBecause:(const char*)why
{
    if (!_capturing) return;
    _lastTapRestart = CACurrentMediaTime();
    wvLog("tap re-open requested: %s (cbs=%llu, frames=%llu, taps=%zu)", why, _cbs.load(),
          _renderedFrames, _slots.size());
    _sampleRate = 0;
    std::string err;
    int ok = 0;
    for (auto& sl : _slots) {
        if ([self openSlot:sl.get() err:err]) ok++;
        else wvLog("tap re-open FAILED for %s: %s", sl->name.c_str(), err.c_str());
    }
    [self reconcileTapsBecause:why];
    _tapRestarts++;
    os_log(wvEngineLog(), "taps re-opened (%{public}s): %d ok, restart #%ld", why, ok, (long)_tapRestarts);
    fprintf(stderr, "[VizEngine] taps re-opened (%s): %d ok\n", why, ok);
    _cbsSeen   = _cbs.load();
    _cbsSeenAt = CACurrentMediaTime();
}

/// Re-open one tap that went quiet while its process still plays.
- (void)reopenSlot:(TapSlot*)slot
{
    std::string err;
    bool dropout = slot->hadSignal.load();
    wvLog("tap re-open requested: %s %s (slot %d)", slot->name.c_str(),
          dropout ? "went silent while playing" : "never produced audio (retry)", slot->index);
    if ([self openSlot:slot err:err]) { if (dropout) _tapRestarts++; }   // the sidebar counts real dropouts only
    else wvLog("tap re-open FAILED for %s: %s", slot->name.c_str(), err.c_str());
    _lastTapRestart = CACurrentMediaTime();
}

- (void)scheduleTapRestartAfter:(double)seconds because:(const char*)why
{
    __weak WVAudioCapture* weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{ [weakSelf restartTapBecause:why]; });
}

- (void)watchdog:(CFTimeInterval)now
{
    unsigned long long c = _cbs.load();
    if (c != _cbsSeen) { _cbsSeen = c; _cbsSeenAt = now; }
    else if (_capturing && !_slots.empty() && _cbsSeenAt > 0 && now - _cbsSeenAt > 3.0 && now - _lastTapRestart > 5.0)
        [self restartTapBecause:"no audio callbacks for 3 s"];
}

- (void)supervise:(CFTimeInterval)now
{
    if (!_capturing) return;
    // Per-tap safety net: a tap that produced audio and then went silent for
    // 1.5 s while its process still reports playing is dead (the macOS 26
    // dropout) — a fresh one is back at once; a track gap costs at most one
    // harmless re-open. A tap that never produced anything belongs to a
    // process that is "playing" silence (WebKit's GPU process, the phone
    // helper) or was dead from the start: retry it with a growing backoff
    // (10 s, 20 s, 40 s … up to ~5 min) so it costs nothing when idle.
    for (auto& sl : _slots) {
        if (sl->peak.load() >= 0.002f) { sl->quietChecks = 0; sl->blindRetries = 0; continue; }
        sl->quietChecks++;
        if (!processRunningOutput(sl->proc)) continue;
        if (sl->hadSignal.load()) {
            if (sl->quietChecks >= 3 && now - sl->lastRestart > 1.9) [self reopenSlot:sl.get()];
        } else {
            double wait = 10.0 * (double)(1 << std::min(sl->blindRetries, 5));
            if (now - sl->lastRestart > wait) { sl->blindRetries++; [self reopenSlot:sl.get()]; }
        }
    }
}

// Sample rate of the default output device and of our aggregate, for the
// log: Apple Music switches the output device's rate per track (44.1/48/96 k
// for lossless), and a tap can go quiet while CoreAudio reconfigures.
- (void)deviceRate:(double *)deviceRate aggregateRate:(double *)aggRate
{
    const AudioObjectID agg = (!_slots.empty() && _slots[0]->tap) ? _slots[0]->tap->aggregateID() : kAudioObjectUnknown;
    *deviceRate = 0; *aggRate = 0;
    AudioObjectID dev = kAudioObjectUnknown; UInt32 sz = sizeof(dev);
    AudioObjectPropertyAddress defAddr = { kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectPropertyAddress rateAddr = { kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &defAddr, 0, nullptr, &sz, &dev) == noErr) {
        Float64 r = 0; sz = sizeof(r);
        if (AudioObjectGetPropertyData(dev, &rateAddr, 0, nullptr, &sz, &r) == noErr) *deviceRate = r;
    }
    if (agg != kAudioObjectUnknown) {
        Float64 r = 0; sz = sizeof(r);
        if (AudioObjectGetPropertyData(agg, &rateAddr, 0, nullptr, &sz, &r) == noErr) *aggRate = r;
    }
}

- (void)installListeners
{
    if (_listenersInstalled) return;
    _listenersInstalled = YES;
    // Default output device changed (headphones, AirPods, a display, …).
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    __weak WVAudioCapture* weakSelf = self;
    _defaultDeviceListener = ^(UInt32, const AudioObjectPropertyAddress*) {
        wvLog("event: default output device changed");
        [weakSelf scheduleTapRestartAfter:0.6 because:"default output device changed"];
    };
    AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &addr,
                                        dispatch_get_main_queue(), _defaultDeviceListener);
    // The set of playing processes changed (an app started or stopped
    // playing, appeared, or quit): the tap only mixes the processes it was
    // built from, so rebuild it when the set really differs. Events come in
    // bursts; only the last one of a burst acts.
    viz::SystemAudioTap::watchProcesses([weakSelf] {
        WVAudioCapture* me = weakSelf; if (!me) return;
        unsigned gen = ++me->_procGen;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.2 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            WVAudioCapture* me2 = weakSelf; if (!me2 || gen != me2->_procGen || !me2->_capturing) return;
            auto now = viz::SystemAudioTap::runningOutputProcesses();
            std::vector<AudioObjectID> have;
            for (auto& sl : me2->_slots) have.push_back(sl->proc);
            std::sort(have.begin(), have.end());
            if (now != have) {
                wvLog("event: playing processes changed (%zu -> %zu)", have.size(), now.size());
                [me2 reconcileTapsBecause:"playing processes changed"];
            }
        });
    });
    // Sleep/wake: the aggregate device rarely survives it.
    _wakeObserver = [[NSWorkspace sharedWorkspace].notificationCenter
        addObserverForName:NSWorkspaceDidWakeNotification object:nil queue:[NSOperationQueue mainQueue]
        usingBlock:^(NSNotification*) {
            wvLog("event: the Mac woke");
            [weakSelf scheduleTapRestartAfter:1.5 because:"the Mac woke"];
        }];
}

- (void)removeListeners
{
    if (!_listenersInstalled) return;
    _listenersInstalled = NO;
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &addr,
                                           dispatch_get_main_queue(), _defaultDeviceListener);
    _defaultDeviceListener = nil;
    viz::SystemAudioTap::watchProcesses(nullptr);
    if (_wakeObserver) {
        [[NSWorkspace sharedWorkspace].notificationCenter removeObserver:_wakeObserver];
        _wakeObserver = nil;
    }
}

@end
