//
// AudioCapture.h — system-audio capture for the engine (Objective-C++,
// private to the engine; not visible to Swift).
//
// One CoreAudio process tap per process that is playing: a tap that mixes
// several processes drops out on macOS 26 (see SystemAudioTap.mm). Each tap
// feeds its own analyzer slot and is watched and re-opened on its own;
// listeners re-open the taps after a default-output-device change, a change
// in which processes play, or the Mac waking. Main thread only.
//
#pragma once
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#include <string>

namespace viz { class Analyzer; }

@interface WVAudioCapture : NSObject

/// `analyzer` receives every tap's audio; it must outlive the capture.
- (instancetype)initWithAnalyzer:(viz::Analyzer *)analyzer NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

/// Opens taps for whatever is playing (more open as other apps start). With
/// nothing playing it makes a throwaway tap so the system permission prompt
/// appears now; `err` says why that failed (most likely: no permission).
- (BOOL)start:(std::string &)err;
- (void)stop;

@property (nonatomic, readonly) BOOL capturing;
@property (nonatomic, readonly) double sampleRate;
@property (nonatomic, readonly) int channels;
/// Re-opens since start: all taps (device change, wake, callbacks stopped)
/// and single taps that went silent while their process kept playing.
@property (nonatomic, readonly) NSInteger tapRestarts;
@property (nonatomic, readonly) NSUInteger tapCount;
/// Audio callbacks so far, all taps together (any thread).
@property (nonatomic, readonly) unsigned long long callbacks;
/// The loudest tap's raw input level, decaying (before analysis).
@property (nonatomic, readonly) float inputPeak;
/// Frames the engine has rendered; only for the log lines written here.
@property (nonatomic) unsigned long long renderedFrames;

/// Every render tick: the tap delivers callbacks continuously, silence
/// included, so none for 3 s means it died; re-open (at most every 5 s).
- (void)watchdog:(CFTimeInterval)now;
/// Twice a second: re-open a tap that went quiet while its process still
/// plays, and retry taps that never produced audio with a growing backoff.
- (void)supervise:(CFTimeInterval)now;
/// For the log: the default output device's and the first tap's aggregate
/// device's nominal sample rates (0 when unknown).
- (void)deviceRate:(double *)deviceRate aggregateRate:(double *)aggregateRate;
/// SIGUSR2 test aid: stop the taps' IO the way a device change does.
- (void)stallForTest;

@end
