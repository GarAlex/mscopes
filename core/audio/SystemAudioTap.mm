//
// SystemAudioTap.mm — CoreAudio process-tap implementation.
//
#import "SystemAudioTap.h"
#import <Foundation/Foundation.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#include <os/log.h>
#include <vector>

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

bool SystemAudioTap::start(std::string& err)
{
    // 1) Describe a global tap of the entire system output (exclude nothing).
    CATapDescription* desc =
        [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
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

    // 3) Build a private aggregate device that includes the tap as a sub-tap.
    NSString* aggUID = [[NSUUID UUID] UUIDString];
    NSString* tapUID = [desc.UUID UUIDString];
    NSDictionary* aggDict = @{
        @(kAudioAggregateDeviceNameKey):        @"MScopesAggregate",
        @(kAudioAggregateDeviceUIDKey):         aggUID,
        @(kAudioAggregateDeviceIsPrivateKey):   @YES,
        @(kAudioAggregateDeviceIsStackedKey):   @NO,
        @(kAudioAggregateDeviceTapAutoStartKey):@YES,
        @(kAudioAggregateDeviceTapListKey): @[ @{
            @(kAudioSubTapUIDKey):              tapUID,
            @(kAudioSubTapDriftCompensationKey):@YES,
        } ],
    };
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
        _channels   = (int)asbd.mChannelsPerFrame;
    } else {
        _sampleRate = 48000; _channels = 2;   // sensible default; log the miss
        os_log_error(tapLog(), "stream format query failed: %{public}@ — assuming 48k/2", fourCC(st));
    }
    os_log(tapLog(), "input format: %.0f Hz, %d ch, flags=0x%x",
           _sampleRate, _channels, (unsigned)asbd.mFormatFlags);

    // 5) IO block: convert whatever layout arrives into interleaved float.
    Callback cb = _cb;
    double sr = _sampleRate;
    __block std::vector<float> scratch;
    AudioDeviceIOBlock ioBlock =
        ^(const AudioTimeStamp*, const AudioBufferList* inData, const AudioTimeStamp*,
          AudioBufferList*, const AudioTimeStamp*)
    {
        if (!inData || inData->mNumberBuffers == 0) return;

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
