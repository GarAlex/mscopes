//
// SystemAudioTap.mm — CoreAudio process-tap implementation.
//
#import "SystemAudioTap.h"
#import <Foundation/Foundation.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#include <os/log.h>
#include <algorithm>
#include <cstdlib>
#include <set>
#include <vector>
#include <unistd.h>

namespace viz {

static os_log_t tapLog() {
    static os_log_t l = os_log_create("com.writea.viz", "tap");
    return l;
}

static NSString* fourCC(OSStatus s) {
    char c[5] = { char((s>>24)&0xff), char((s>>16)&0xff), char((s>>8)&0xff), char(s&0xff), 0 };
    // printable? else numeric
    for (int i = 0; i < 4; ++i) if (c[i] < 32 || c[i] > 126) return [NSString stringWithFormat:@"%d", (int)s];
    return [NSString stringWithFormat:@"'%s' (%d)", c, (int)s];
}

SystemAudioTap::~SystemAudioTap() { stop(); }

static std::vector<AudioObjectID> allProcessObjects()
{
    std::vector<AudioObjectID> objs;
    AudioObjectPropertyAddress la = { kAudioHardwarePropertyProcessObjectList,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &la, 0, nullptr, &sz) != noErr || !sz) return objs;
    objs.resize(sz / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &la, 0, nullptr, &sz, objs.data()) != noErr) objs.clear();
    objs.resize(sz / sizeof(AudioObjectID));
    return objs;
}

std::vector<AudioObjectID> SystemAudioTap::runningOutputProcesses()
{
    std::vector<AudioObjectID> out;
    pid_t me = getpid();
    for (AudioObjectID o : allProcessObjects()) {
        pid_t pid = 0; UInt32 psz = sizeof(pid);
        AudioObjectPropertyAddress pa = { kAudioProcessPropertyPID,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(o, &pa, 0, nullptr, &psz, &pid) == noErr && pid == me) continue;
        UInt32 running = 0, rsz = sizeof(running);
        AudioObjectPropertyAddress ra = { kAudioProcessPropertyIsRunningOutput,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(o, &ra, 0, nullptr, &rsz, &running) == noErr && running)
            out.push_back(o);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Process watching: one listener on the system's process list, plus one on
// every process object's running-output flag (re-installed as the list
// changes). All fire on the main queue.
static std::function<void()>              gProcHandler;
static AudioObjectPropertyListenerBlock   gProcListListener = nil;
static AudioObjectPropertyListenerBlock   gProcRunListener  = nil;
static std::set<AudioObjectID>            gWatchedProcs;

static void installPerProcessListeners()
{
    AudioObjectPropertyAddress ra = { kAudioProcessPropertyIsRunningOutput,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    std::set<AudioObjectID> now;
    for (AudioObjectID o : allProcessObjects()) now.insert(o);
    for (AudioObjectID o : now)
        if (!gWatchedProcs.count(o))
            AudioObjectAddPropertyListenerBlock(o, &ra, dispatch_get_main_queue(), gProcRunListener);
    for (AudioObjectID o : gWatchedProcs)
        if (!now.count(o))
            AudioObjectRemovePropertyListenerBlock(o, &ra, dispatch_get_main_queue(), gProcRunListener);
    gWatchedProcs.swap(now);
}

void SystemAudioTap::watchProcesses(std::function<void()> handler)
{
    AudioObjectPropertyAddress la = { kAudioHardwarePropertyProcessObjectList,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    if (gProcListListener) {
        AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &la, dispatch_get_main_queue(), gProcListListener);
        gProcListListener = nil;
        AudioObjectPropertyAddress ra = { kAudioProcessPropertyIsRunningOutput,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        for (AudioObjectID o : gWatchedProcs)
            AudioObjectRemovePropertyListenerBlock(o, &ra, dispatch_get_main_queue(), gProcRunListener);
        gWatchedProcs.clear();
        gProcRunListener = nil;
    }
    gProcHandler = std::move(handler);
    if (!gProcHandler) return;
    gProcRunListener = ^(UInt32, const AudioObjectPropertyAddress*) { if (gProcHandler) gProcHandler(); };
    gProcListListener = ^(UInt32, const AudioObjectPropertyAddress*) {
        installPerProcessListeners();
        if (gProcHandler) gProcHandler();
    };
    AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &la, dispatch_get_main_queue(), gProcListListener);
    installPerProcessListeners();
}

bool SystemAudioTap::start(std::string& err)
{
    // 1) Describe the tap. Two ways to tap "everything":
    //    - a global tap (initStereoGlobalTapButExcludeProcesses:@[]) — on
    //      macOS 26 this mode goes silent for seconds to minutes every few
    //      minutes while the audio plays on (measured against a process
    //      tap running side by side), so it is only kept for comparison
    //      (MSCOPES_TAP=global);
    //    - a mixdown of the processes that are producing output right now
    //      (a mixdown of *all* 30-odd process objects dropped out the same
    //      way; a tap on the one playing process stayed clean for as long as
    //      it ran). The set changes as apps start and stop playing, so the
    //      engine watches it (watchProcesses) and re-opens the tap.
    NSMutableArray<NSNumber*>* procs = [NSMutableArray array];
    const char* mode = getenv("MSCOPES_TAP");
    _procs.clear();
    if (!(mode && strcmp(mode, "global") == 0)) {
        _procs = runningOutputProcesses();
        for (AudioObjectID o : _procs) [procs addObject:@(o)];
    }
    CATapDescription* desc = procs.count
        ? [[CATapDescription alloc] initStereoMixdownOfProcesses:procs]
        : [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
    _processCount = (int)procs.count;
    os_log(tapLog(), "tap mode: %{public}s of %lu playing process(es)", procs.count ? "mixdown" : "global (none playing)", (unsigned long)procs.count);
    fprintf(stderr, "[tap] mode: %s (%lu playing process objects)\n", procs.count ? "mixdown" : "global", (unsigned long)procs.count);
    desc.name = @"MScopesSystemTap";
    desc.muteBehavior = CATapUnmuted;     // keep audio audible while we tap it
    desc.privateTap = YES;                // don't publish to other apps

    // 2) Create the tap object.
    OSStatus st = AudioHardwareCreateProcessTap(desc, &_tapID);
    if (st != noErr || _tapID == kAudioObjectUnknown) {
        err = [[NSString stringWithFormat:@"AudioHardwareCreateProcessTap failed: %@", fourCC(st)] UTF8String];
        os_log_error(tapLog(), "%{public}s", err.c_str());
        return false;
    }
    os_log(tapLog(), "created tap id=%u", _tapID);

    // The tap's own format: its channel count identifies its buffers below.
    int tapChannels = 2;
    {
        AudioStreamBasicDescription tf = {}; UInt32 tsz = sizeof(tf);
        AudioObjectPropertyAddress tfAddr = { kAudioTapPropertyFormat,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(_tapID, &tfAddr, 0, nullptr, &tsz, &tf) == noErr && tf.mChannelsPerFrame > 0)
            tapChannels = (int)tf.mChannelsPerFrame;
    }

    // 3) Build a private aggregate device: the default OUTPUT device as its
    //    main sub-device — so the aggregate runs on that device's clock —
    //    plus the tap. Without the sub-device the aggregate free-runs on a
    //    clock of its own and periodically drifts out of step with the real
    //    device, which showed up as a few seconds of silence every few
    //    minutes while music played on.
    NSString* aggUID = [[NSUUID UUID] UUIDString];
    NSString* tapUID = [desc.UUID UUIDString];
    NSString* outUID = nil; NSString* outName = @"?";
    {
        AudioObjectID outDev = kAudioObjectUnknown; UInt32 dsz = sizeof(outDev);
        AudioObjectPropertyAddress defAddr = { kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &defAddr, 0, nullptr, &dsz, &outDev) == noErr
            && outDev != kAudioObjectUnknown) {
            CFStringRef uid = nullptr; dsz = sizeof(uid);
            AudioObjectPropertyAddress uidAddr = { kAudioDevicePropertyDeviceUID,
                kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
            if (AudioObjectGetPropertyData(outDev, &uidAddr, 0, nullptr, &dsz, &uid) == noErr && uid)
                outUID = CFBridgingRelease(uid);
            CFStringRef name = nullptr; dsz = sizeof(name);
            AudioObjectPropertyAddress nameAddr = { kAudioObjectPropertyName,
                kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
            if (AudioObjectGetPropertyData(outDev, &nameAddr, 0, nullptr, &dsz, &name) == noErr && name)
                outName = CFBridgingRelease(name);
        }
    }
    NSMutableDictionary* aggDict = [@{
        @(kAudioAggregateDeviceNameKey):        @"MScopesAggregate",
        @(kAudioAggregateDeviceUIDKey):         aggUID,
        @(kAudioAggregateDeviceIsPrivateKey):   @YES,
        @(kAudioAggregateDeviceIsStackedKey):   @NO,
        @(kAudioAggregateDeviceTapAutoStartKey):@YES,
        @(kAudioAggregateDeviceTapListKey): @[ @{
            @(kAudioSubTapUIDKey):              tapUID,
            // Off: the aggregate is clocked by the very device the tap
            // follows, so there is nothing to compensate — and the
            // resampler it inserts is a suspect for multi-second silences.
            @(kAudioSubTapDriftCompensationKey):@NO,
        } ],
    } mutableCopy];
    if (outUID) {
        aggDict[@(kAudioAggregateDeviceMainSubDeviceKey)] = outUID;
        aggDict[@(kAudioAggregateDeviceSubDeviceListKey)] = @[ @{ @(kAudioSubDeviceUIDKey): outUID } ];
        os_log(tapLog(), "clocking the aggregate by the default output device \"%{public}@\"", outName);
        fprintf(stderr, "[tap] aggregate clocked by \"%s\"\n", outName.UTF8String);
    } else {
        os_log_error(tapLog(), "no default output device UID — aggregate will free-run");
    }
    st = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggDict, &_aggID);
    if (st != noErr || _aggID == kAudioObjectUnknown) {
        err = [[NSString stringWithFormat:@"AudioHardwareCreateAggregateDevice failed: %@", fourCC(st)] UTF8String];
        os_log_error(tapLog(), "%{public}s", err.c_str());
        stop();
        return false;
    }
    os_log(tapLog(), "created aggregate id=%u", _aggID);

    // 4) Query the input stream format (sample rate / channels).
    AudioStreamBasicDescription asbd = {};
    UInt32 sz = sizeof(asbd);
    AudioObjectPropertyAddress fmtAddr = {
        kAudioDevicePropertyStreamFormat,
        kAudioObjectPropertyScopeInput,
        kAudioObjectPropertyElementMain
    };
    st = AudioObjectGetPropertyData(_aggID, &fmtAddr, 0, nullptr, &sz, &asbd);
    if (st == noErr) {
        _sampleRate = asbd.mSampleRate;
        _channels   = tapChannels;   // the first input stream may be the sub-device's own mic
    } else {
        _sampleRate = 48000; _channels = 2;   // sensible default; log the miss
        os_log_error(tapLog(), "stream format query failed: %{public}@ — assuming 48k/2", fourCC(st));
    }
    os_log(tapLog(), "input format: %.0f Hz, %d ch, flags=0x%x",
           _sampleRate, _channels, (unsigned)asbd.mFormatFlags);

    // 5) IO block: hand the tap's buffer on as interleaved float. The
    //    aggregate's input also carries the sub-device's own input streams
    //    (a display's or AirPods' microphone) — those come first; the tap's
    //    buffer is the last one with the tap's channel count.
    Callback cb = _cb;
    double sr = _sampleRate;
    __block std::vector<float> scratch;
    __block bool layoutLogged = false;
    AudioDeviceIOBlock ioBlock =
        ^(const AudioTimeStamp*, const AudioBufferList* inData, const AudioTimeStamp*,
          AudioBufferList*, const AudioTimeStamp*)
    {
        if (!inData || inData->mNumberBuffers == 0) return;

        int tapBuf = -1;
        for (int i = (int)inData->mNumberBuffers - 1; i >= 0; --i)
            if ((int)inData->mBuffers[i].mNumberChannels == tapChannels) { tapBuf = i; break; }
        if (!layoutLogged) {
            layoutLogged = true;
            os_log(tapLog(), "input layout: %u buffer(s), tap = #%d (%d ch)",
                   inData->mNumberBuffers, tapBuf, tapChannels);
            fprintf(stderr, "[tap] input layout: %u buffer(s), tap = #%d (%d ch)\n",
                    inData->mNumberBuffers, tapBuf, tapChannels);
        }
        if (tapBuf >= 0) {
            const AudioBuffer& buf = inData->mBuffers[tapBuf];
            int ch = (int)buf.mNumberChannels;
            int frames = (int)(buf.mDataByteSize / sizeof(float) / ch);
            cb((const float*)buf.mData, frames, ch, sr);
            return;
        }

        if (inData->mNumberBuffers == 1) {
            // interleaved (mNumberChannels channels in one buffer)
            const AudioBuffer& buf = inData->mBuffers[0];
            int ch = (int)buf.mNumberChannels ? (int)buf.mNumberChannels : 1;
            int frames = (int)(buf.mDataByteSize / sizeof(float) / ch);
            cb((const float*)buf.mData, frames, ch, sr);
        } else {
            // deinterleaved: one buffer per channel → interleave into scratch
            int ch = (int)inData->mNumberBuffers;
            int frames = (int)(inData->mBuffers[0].mDataByteSize / sizeof(float));
            if ((int)scratch.size() < frames * ch) scratch.resize(frames * ch);
            for (int c = 0; c < ch; ++c) {
                const float* src = (const float*)inData->mBuffers[c].mData;
                for (int n = 0; n < frames; ++n) scratch[n * ch + c] = src[n];
            }
            cb(scratch.data(), frames, ch, sr);
        }
    };

    st = AudioDeviceCreateIOProcIDWithBlock(&_procID, _aggID, nullptr, ioBlock);
    if (st != noErr || !_procID) {
        err = [[NSString stringWithFormat:@"AudioDeviceCreateIOProcIDWithBlock failed: %@", fourCC(st)] UTF8String];
        os_log_error(tapLog(), "%{public}s", err.c_str());
        stop();
        return false;
    }

    st = AudioDeviceStart(_aggID, _procID);
    if (st != noErr) {
        err = [[NSString stringWithFormat:@"AudioDeviceStart failed: %@", fourCC(st)] UTF8String];
        os_log_error(tapLog(), "%{public}s", err.c_str());
        stop();
        return false;
    }

    _running = true;
    os_log(tapLog(), "tap running");
    return true;
}

void SystemAudioTap::pauseForTest()
{
    if (_aggID != kAudioObjectUnknown && _procID) {
        AudioDeviceStop(_aggID, _procID);
        os_log(tapLog(), "paused for test: IO stopped, objects kept");
    }
}

void SystemAudioTap::stop()
{
    if (_aggID != kAudioObjectUnknown && _procID) {
        AudioDeviceStop(_aggID, _procID);
        AudioDeviceDestroyIOProcID(_aggID, _procID);
        _procID = nullptr;
    }
    if (_aggID != kAudioObjectUnknown) {
        AudioHardwareDestroyAggregateDevice(_aggID);
        _aggID = kAudioObjectUnknown;
    }
    if (_tapID != kAudioObjectUnknown) {
        AudioHardwareDestroyProcessTap(_tapID);
        _tapID = kAudioObjectUnknown;
    }
    _running = false;
}

} // namespace viz
