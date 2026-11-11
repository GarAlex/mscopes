//
// VizEngine.mm — Objective-C++ implementation bridging the C++ core to Swift:
// presets and settings, the frame loop (display link → analyse → render →
// present) and its supervision. Its parts:
//   AudioCapture.mm   the system-audio taps feeding the analyzer
//   WVRenderView.mm   the view the frames are presented in
//   EngineLog.mm      ~/Library/Logs/MScopes/engine.log
//
// Compiled under ARC (Xcode default). The C++ members (Analyzer, effect host)
// are constructed/destructed with the ObjC object.
//
#import "VizEngine.h"
#import <QuartzCore/QuartzCore.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include "VizFrame.h"
#include "GpuFx.h"
#include "Picture.h"
#include "LineMode.h"
#import  "Analyzer.h"
#import  "AudioCapture.h"
#import  "WVRenderView.h"
#include "EngineLog.h"
#include <algorithm>
#include <memory>
#include <vector>
#include "EffectHost.h"
#include "BuiltinEffects.h"
#include "Presets.h"
#include "AvsPreset.h"
#include "JsonPreset.h"
#include "EelVM.h"
#include "Superscope.h"
#include "DynamicMovement.h"
#include "ScriptedTrans.h"

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <csignal>

// SIGUSR1 → snapshot the whole app window to PNG (dev aid; captures SwiftUI too).
static std::atomic<bool> gWantWindowSnapshot{false};
static void wvOnSIGUSR1(int) { gWantWindowSnapshot = true; }

// SIGUSR2: test aid for the tap watchdog — stops the tap's IO the way a
// device change or a sleep does, then the engine must recover on its own.
@interface VizEngine ()
- (void)debugStallTap;
@end
static VizEngine* gEngineForSignals = nil;
static void wvOnSIGUSR2(int)
{
    dispatch_async(dispatch_get_main_queue(), ^{ [gEngineForSignals debugStallTap]; });
}


// ---------------------------------------------------------------------------
//  VizEngine
// ---------------------------------------------------------------------------
// Internal render resolution for the CPU effect host; the blit scales it to the
// view. Classic-AVS approach: effects run cheap, upscale does the rest.
static const int kFxW = 480, kFxH = 270;

@interface VizEngine ()
- (void)scheduleTick;
@end

static CVReturn wvDisplayLinkFired(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*,
                                   CVOptionFlags, CVOptionFlags*, void* ctx);

@implementation VizEngine {
    // Picture (display-stage looks, Picture.h): applied only when the frame
    // is drawn to the window, never written into the frame presets keep.
    viz::PictureSettings  _pic;
    viz::LookTable        _builtinLook;    // the selected built-in look, baked
    viz::LookTable        _customLook;     // a loaded .cube look
    NSInteger             _lookIndex;      // 0 = Off, -1 = custom
    NSString*             _customLookName;
    // Draw quality (LineMode.h DrawQuality) and render resolution. Classic
    // presets (.avs files) get them only when "Classic presets: Enhanced".
    BOOL                  _smoothLines;
    BOOL                  _highResolution;
    BOOL                  _classicEnhanced;
    BOOL                  _presetIsClassic;
    BOOL                  _highResBlocked;   // this preset missed the frame budget at high res
    BOOL                  _hdrHighlights;
    int                   _hits[3];          // kick/snare/hat hits since the last heartbeat
    BOOL                  _windowHidden;     // drawing paused: the window can't be seen
    float                 _att[3];           // slow lows/mids/highs of the latest frame
    double                _renderMsAvg;
    int                   _overBudgetFrames;
    viz::Analyzer         _analyzer;
    WVAudioCapture*       _capture;        // taps → _analyzer (AudioCapture.h)
    WVRenderView*         _view;
    NSInteger             _linkRestarts;   // display link rebuilt (ticks stopped, screens changed)
    unsigned long long    _frames;         // ticks rendered since capture started
    NSTimer*              _supervisor;     // 1 Hz: display-link watchdog + heartbeat
    id                    _screenObserver;
    // Frame pacing: a CVDisplayLink fires on its own thread exactly at vsync
    // and hands the tick to the (idle) main thread. The AppKit-integrated
    // NSView displayLink was tried first and measured: it is delivered inside
    // the CA transaction flush, and whenever the callback runs long (~10 ms
    // of CPU effects) AppKit defers the next one a whole vsync — half the
    // frames landed 33 ms apart on a 60 Hz panel with the main thread idle.
    CVDisplayLinkRef      _cvLink;
    NSTimer*              _fallbackTimer;
    std::atomic<bool>     _tickPending;
    CFTimeInterval        _lastTick;
    // pacing diagnostics (written on the vsync thread, read on main; racy by design)
    CFTimeInterval        _cbTime;        // when the last display-link callback fired
    CFTimeInterval        _cbPrev;
    int                   _cbLate;        // callbacks > 25 ms after the previous one
    double                _cbLatencyMax;  // callback → tick start (ms)

    viz::EffectHost       _host;
    viz::EffectContext    _fxCtx;
    int                   _aspectMode;   // 0 stretch, 1 fill, 2 fit
    NSInteger             _currentPreset;

    float  _peak, _bass, _mid, _treble;
    float  _bpm, _beatLevel;
    float  _bands[32];                  // log-spaced bands of the latest frame
}

- (instancetype)init
{
    if ((self = [super init])) {
        _view = [[WVRenderView alloc] initWithFrame:NSMakeRect(0, 0, 640, 360)];
        _view->showHUD = NO;
        _sensitivity = 1.0f;
        _lookIndex = 0;
        _view->pic = &_pic;
        _smoothLines = YES;
        _hdrHighlights = YES;
        _highResolution = YES;
        _classicEnhanced = NO;
        _presetIsClassic = NO;
        _aspectMode = 1;               // fill: shapes stay round fullscreen
        _capture = [[WVAudioCapture alloc] initWithAnalyzer:&_analyzer];
        gEngineForSignals = self;
        signal(SIGUSR1, wvOnSIGUSR1);
        signal(SIGUSR2, wvOnSIGUSR2);
        // MSCOPES_SNAPSHOT_AFTER=<seconds>: one snapshot after that long,
        // whether or not capture is running (e.g. of the first-launch screen).
        if (const char* after = getenv("MSCOPES_SNAPSHOT_AFTER")) {
            __weak VizEngine* weakSelf = self;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(atof(after) * NSEC_PER_SEC)),
                           dispatch_get_main_queue(), ^{ [weakSelf writeSnapshots]; });
        }

        // Start on the first built-in preset — or, for scripted runs and
        // profiling from the shell, on MSCOPES_PRESET=<file.avs|.json>.
        _host.resize(kFxW, kFxH);
        _currentPreset = 0;
        viz::applyPreset(_host, viz::builtinPresets()[0]);
        if (const char* p = getenv("MSCOPES_PRESET")) {
            // A built-in preset's name, or a path to a .avs/.json file.
            NSString* path = [NSString stringWithUTF8String:p];
            NSString* rep = nil;
            const auto& builtins = viz::builtinPresets();
            for (size_t i = 0; i < builtins.size(); ++i)
                if (builtins[i].name == p) {
                    _currentPreset = (NSInteger)i;
                    viz::applyPreset(_host, builtins[i]);
                    rep = @"built-in";
                }
            if (!rep)
                rep = [path.pathExtension.lowercaseString isEqualToString:@"json"]
                    ? [self loadJsonPresetAtPath:path] : [self loadAvsPresetAtPath:path];
            fprintf(stderr, "[VizEngine] MSCOPES_PRESET %s: %s\n", p, rep.UTF8String);
        }
        _view->fb = &_host.current();
    }
    return self;
}

- (void)dealloc { [self stop]; }

- (NSView *)renderView { return _view; }
- (BOOL)isCapturing    { return _capture.capturing; }
- (double)sampleRate   { return _capture.sampleRate; }
- (NSInteger)channels  { return _capture.channels; }
- (float)peak          { return _peak; }
- (float)bass          { return _bass; }
- (float)mid           { return _mid; }
- (float)treble        { return _treble; }
- (float)bpm           { return _bpm; }
- (float)beatLevel     { return _beatLevel; }
- (void)copyBands:(float *)out count:(NSInteger)count
{
    // Resample our 32 fixed bands onto the caller's count (nearest).
    count = std::max<NSInteger>(1, std::min<NSInteger>(32, count));
    for (NSInteger i = 0; i < count; ++i)
        out[i] = _bands[(i * 32) / count];
}
- (unsigned long long)audioCallbacks { return _capture.callbacks; }

- (void)setShowHUD:(BOOL)showHUD { _showHUD = showHUD; _view->showHUD = showHUD; }

// ---- Picture ------------------------------------------------------------
- (NSArray<NSString *> *)lookNames
{
    NSMutableArray* a = [NSMutableArray array];
    for (const auto& n : viz::lookNames()) [a addObject:@(n.c_str())];
    return a;
}
- (NSInteger)lookIndex { return _lookIndex; }
- (void)setLookIndex:(NSInteger)i
{
    if (i == -1) {
        if (_customLook.empty()) return;          // nothing loaded: keep the current look
        _lookIndex = -1;
        _pic.look = &_customLook;
        return;
    }
    if (!viz::buildLook((int)i, _builtinLook)) return;
    _lookIndex = i;
    _pic.look = (i == 0) ? nullptr : &_builtinLook;
}
- (nullable NSString *)loadCustomLookAtPath:(NSString *)path
{
    std::string err;
    viz::LookTable t;
    if (!viz::loadCubeFile(path.fileSystemRepresentation, t, &err))
        return [NSString stringWithUTF8String:err.c_str()];
    _customLook = std::move(t);
    _customLookName = [[path lastPathComponent] stringByDeletingPathExtension];
    _lookIndex = -1;
    _pic.look = &_customLook;
    return nil;
}
- (nullable NSString *)customLookName { return _customLook.empty() ? nil : _customLookName; }
- (float)lookStrength { return _pic.lookStrength; }
- (void)setLookStrength:(float)v { _pic.lookStrength = std::clamp(v, 0.f, 1.f); }
- (float)grain { return _pic.grain; }
- (void)setGrain:(float)v { _pic.grain = std::clamp(v, 0.f, 1.f); }
- (float)vignette { return _pic.vignette; }
- (void)setVignette:(float)v { _pic.vignette = std::clamp(v, 0.f, 1.f); }
- (float)glow { return _pic.glow; }
- (void)setGlow:(float)v { _pic.glow = std::clamp(v, 0.f, 1.f); }
- (float)scanlines { return _pic.scanlines; }
- (void)setScanlines:(float)v { _pic.scanlines = std::clamp(v, 0.f, 1.f); }
- (BOOL)pictureReactsToBeat { return _pic.beatReactive; }
- (void)setPictureReactsToBeat:(BOOL)b { _pic.beatReactive = b; }
- (BOOL)sharpScaling { return _pic.sharpScaling; }
- (void)setSharpScaling:(BOOL)b { _pic.sharpScaling = b; }

- (NSArray<NSString *> *)presetNames
{
    NSMutableArray* a = [NSMutableArray array];
    for (const auto& p : viz::builtinPresets())
        [a addObject:[NSString stringWithFormat:@"%s|%d", p.name.c_str(), p.modern ? 1 : 0]];
    return a;
}

- (NSInteger)currentPreset { return _currentPreset; }

- (void)setCurrentPreset:(NSInteger)index
{
    const auto& presets = viz::builtinPresets();
    if (index < 0 || (size_t)index >= presets.size()) return;
    _currentPreset = index;
    _host.beginCrossfade(1.2);            // Milkdrop-style preset blend
    viz::applyPreset(_host, presets[(size_t)index]);
    [self presetKindChanged:NO];
}

- (NSArray<NSString *> *)effectNames
{
    NSMutableArray* a = [NSMutableArray array];
    for (size_t i = 0; i < _host.count(); ++i)
        [a addObject:[NSString stringWithUTF8String:_host.at(i)->name()]];
    return a;
}

- (BOOL)isEffectEnabledAt:(NSInteger)index
{
    viz::Effect* e = _host.at((size_t)index);
    return e ? e->enabled : NO;
}

- (void)setEffect:(NSInteger)index enabled:(BOOL)enabled
{
    if (viz::Effect* e = _host.at((size_t)index)) e->enabled = enabled;
}

- (NSArray<NSString *> *)availableEffects
{
    // "key|Display Name", sorted by display name. Instantiation is cheap.
    NSMutableArray* a = [NSMutableArray array];
    for (const auto& [key, factory] : viz::effectRegistry()) {
        auto fx = factory();
        [a addObject:[NSString stringWithFormat:@"%s|%s|%d", key.c_str(), fx->name(),
                      viz::isModernEffectKey(key) ? 1 : 0]];
    }
    [a sortUsingComparator:^NSComparisonResult(NSString* x, NSString* y) {
        NSString* nx = [x componentsSeparatedByString:@"|"].lastObject;
        NSString* ny = [y componentsSeparatedByString:@"|"].lastObject;
        return [nx compare:ny];
    }];
    return a;
}

- (void)addEffectWithKey:(NSString *)key
{
    const auto& reg = viz::effectRegistry();
    auto it = reg.find(std::string(key.UTF8String));
    if (it != reg.end()) _host.add(it->second());
}

- (void)removeEffectAt:(NSInteger)index { _host.removeAt((size_t)index); }

- (NSArray<NSString *> *)paramsForEffectAt:(NSInteger)index
{
    NSMutableArray* a = [NSMutableArray array];
    if (viz::Effect* e = _host.at((size_t)index)) {
        for (const auto& p : e->params())
            [a addObject:[NSString stringWithFormat:@"%s|%g|%g|%g|%d",
                          p.key, p.minV, p.maxV, *p.value,
                          _host.bindingFor((size_t)index, p.key)]];
    }
    return a;
}

- (void)bindParamForEffectAt:(NSInteger)index key:(NSString *)key toReg:(NSInteger)reg
{
    _host.setBinding((size_t)index, std::string(key.UTF8String), (int)reg);
}

- (void)setParamForEffectAt:(NSInteger)index key:(NSString *)key value:(float)value
{
    if (viz::Effect* e = _host.at((size_t)index))
        e->setParam(std::string(key.UTF8String), value);
}

// std::string → NSString that never returns nil: classic .avs scripts are
// often Latin-1, which stringWithUTF8String: rejects (and a nil in an @[]
// literal throws NSInvalidArgumentException).
static NSString* safeNS(const std::string& s)
{
    if (NSString* r = [NSString stringWithUTF8String:s.c_str()]) return r;
    NSString* r = [[NSString alloc] initWithData:[NSData dataWithBytes:s.data() length:s.size()]
                                        encoding:NSISOLatin1StringEncoding];
    return r ?: @"";
}

- (NSArray<NSString *> *)scriptsForEffectAt:(NSInteger)index
{
    viz::Effect* e = _host.at((size_t)index);
    std::string i, f, b, p;
    if (auto* ss = dynamic_cast<viz::SuperscopeEffect*>(e)) ss->getScripts(i, f, b, p);
    else if (auto* dm = dynamic_cast<viz::DynamicMovementEffect*>(e)) dm->getScripts(i, f, b, p);
    else if (auto* st = dynamic_cast<viz::ScriptedEffectBase*>(e)) st->getScripts(i, f, b, p);
    else return nil;
    return @[ safeNS(i), safeNS(f), safeNS(b), safeNS(p) ];
}

- (void)setScriptsForEffectAt:(NSInteger)index
                         initScript:(NSString *)i frameScript:(NSString *)f
                         beatScript:(NSString *)b pointScript:(NSString *)p
{
    viz::Effect* e = _host.at((size_t)index);
    auto s = [](NSString* x) { return std::string(x.UTF8String); };
    if (auto* ss = dynamic_cast<viz::SuperscopeEffect*>(e))
        ss->setScripts(s(i), s(f), s(b), s(p));
    else if (auto* dm = dynamic_cast<viz::DynamicMovementEffect*>(e))
        dm->setScripts(s(i), s(f), s(b), s(p));
    else if (auto* st = dynamic_cast<viz::ScriptedEffectBase*>(e))
        st->setScripts(s(i), s(f), s(b), s(p));
}

- (NSString *)loadAvsPresetAtPath:(NSString *)path
{
    viz::AvsLoadReport rep;
    _host.beginCrossfade(1.2);
    viz::loadAvsPresetFile(_host, std::string(path.UTF8String), rep);
    [self presetKindChanged:YES];
    return safeNS(rep.summary());
}

// Album/sequence playback: track 1 opens normally (crossfade in, own
// clear-vs-inherit classification); every later track loads with NO
// crossfade and forced inheritance — real AVS never clears between preset
// switches, and these numbered series are authored as one continuous show.
- (NSString *)loadAlbumTrackAtPath:(NSString *)path isFirst:(BOOL)isFirst
{
    viz::AvsLoadReport rep;
    if (isFirst) {
        _host.beginCrossfade(1.2);
        viz::loadAvsPresetFile(_host, std::string(path.UTF8String), rep, false);
        [self presetKindChanged:YES];
    } else {
        viz::loadAvsPresetFile(_host, std::string(path.UTF8String), rep, true);
    }
    return safeNS(rep.summary());
}

// A new preset: note whether it is a classic .avs one (quality settings
// apply to those only in Enhanced mode) and give high resolution a fresh try.
- (void)presetKindChanged:(BOOL)classic
{
    _presetIsClassic = classic;
    _highResBlocked = NO;
    _overBudgetFrames = 0;
}

- (BOOL)smoothLines { return _smoothLines; }
- (void)setSmoothLines:(BOOL)b { _smoothLines = b; }
- (BOOL)highResolution { return _highResolution; }
- (void)setHighResolution:(BOOL)b { _highResolution = b; _highResBlocked = NO; _overBudgetFrames = 0; }
- (BOOL)classicEnhanced { return _classicEnhanced; }
- (void)setClassicEnhanced:(BOOL)b { _classicEnhanced = b; _highResBlocked = NO; _overBudgetFrames = 0; }
- (BOOL)presetIsClassic { return _presetIsClassic; }
- (BOOL)hdrHighlights { return _hdrHighlights; }
- (BOOL)highResolutionReduced
{
    return _highResolution && _highResBlocked && (!_presetIsClassic || _classicEnhanced);
}
- (void)setHdrHighlights:(BOOL)b { _hdrHighlights = b; }

- (void)setAspectMode:(NSInteger)mode { _aspectMode = (int)mode; }
- (NSInteger)aspectMode { return _aspectMode; }

- (void)setGlobalReg:(NSInteger)index value:(float)value
{
    _host.activate();
    *viz::eel::VM::globalRegSlot((int)index) = value;
}
- (float)globalReg:(NSInteger)index
{
    _host.activate();
    return (float)*viz::eel::VM::globalRegSlot((int)index);
}

- (NSString *)saveJsonPresetToPath:(NSString *)path name:(NSString *)name
{
    bool ok = viz::saveJsonPresetFile(_host, std::string(name.UTF8String),
                                      std::string(path.UTF8String));
    return ok ? [NSString stringWithFormat:@"saved %@", name]
              : [NSString stringWithFormat:@"could not write %@", path];
}

- (NSString *)loadJsonPresetAtPath:(NSString *)path
{
    std::string err;
    _host.beginCrossfade(1.2);
    bool ok = viz::loadJsonPresetFile(_host, std::string(path.UTF8String), &err);
    [self presetKindChanged:NO];
    return ok ? [NSString stringWithFormat:@"loaded, %zu effects", _host.count()]
              : safeNS(err);
}
- (void)moveEffectAt:(NSInteger)from to:(NSInteger)to
{
    if (from < 0 || to < 0) return;
    _host.move((size_t)from, (size_t)to);
}

- (void)debugStallTap { [_capture stallForTest]; }

// MARK: - render loop supervision

- (void)startDisplayLink
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // CVDisplayLink: see ivar note
    if (CVDisplayLinkCreateWithActiveCGDisplays(&_cvLink) == kCVReturnSuccess) {
        CVDisplayLinkSetOutputCallback(_cvLink, wvDisplayLinkFired, (__bridge void*)self);
        CVDisplayLinkStart(_cvLink);
    } else {
        fprintf(stderr, "[VizEngine] CVDisplayLink unavailable; falling back to a 60 Hz timer\n");
        wvLog("CVDisplayLink unavailable; 60 Hz timer fallback");
        NSTimer* t = [NSTimer timerWithTimeInterval:1.0/60.0 target:self
                                           selector:@selector(tick:) userInfo:nil repeats:YES];
        [[NSRunLoop mainRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
        _fallbackTimer = t;
    }
#pragma clang diagnostic pop
}

- (void)stopDisplayLink
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (_cvLink) { CVDisplayLinkStop(_cvLink); CVDisplayLinkRelease(_cvLink); _cvLink = nullptr; }
#pragma clang diagnostic pop
    [_fallbackTimer invalidate]; _fallbackTimer = nil;
}

- (void)restartDisplayLinkBecause:(const char*)why
{
    if (!_capture.capturing) return;
    [self stopDisplayLink];
    [self startDisplayLink];
    _linkRestarts++;
    _lastTick = CACurrentMediaTime();
    wvLog("display link rebuilt (%s), restart #%ld", why, (long)_linkRestarts);
    fprintf(stderr, "[VizEngine] display link rebuilt (%s)\n", why);
}

// Displays changed (one slept, was unplugged, changed mode): the
// CVDisplayLink may keep running or may not — rebuild it to be sure.
- (void)installScreenObserver
{
    if (_screenObserver) return;
    __weak VizEngine* weakSelf = self;
    _screenObserver = [[NSNotificationCenter defaultCenter]
        addObserverForName:NSApplicationDidChangeScreenParametersNotification object:nil
        queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification*) {
            wvLog("event: screen parameters changed");
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                           dispatch_get_main_queue(), ^{ [weakSelf restartDisplayLinkBecause:"screen parameters changed"]; });
        }];
}

- (void)removeScreenObserver
{
    if (!_screenObserver) return;
    [[NSNotificationCenter defaultCenter] removeObserver:_screenObserver];
    _screenObserver = nil;
}

/// Twice a second: rebuild the display link if ticks stopped, re-open a
/// silent tap, and write the heartbeat every 10 s.
- (void)supervise
{
    if (!_capture.capturing) return;
    CFTimeInterval now = CACurrentMediaTime();
    [_capture supervise:now];
    if (_lastTick > 0 && now - _lastTick > 2.0)
        [self restartDisplayLinkBecause:"no render ticks for 2 s"];
    // Audio going silent while callbacks keep coming is the other way the
    // visuals can stop; note when it starts and ends (with the counters).
    static int quiet = 0; static bool wasSilent = false; static CFTimeInterval silentSince = 0;
    if (_peak < 0.002f) {
        // Safety net for the macOS 26 tap dropouts: silence from the tap
        // while some process still reports it is playing means the tap
        // died, not the music — a fresh tap comes back immediately.
        if (++quiet == 3 && !wasSilent) {
            wasSilent = true; silentSince = now;
            double dr, ar; [_capture deviceRate:&dr aggregateRate:&ar];
            wvLog("audio silent (cbs=%llu frames=%llu bpm=%.0f input peak %.4f) device %.0f Hz, aggregate %.0f Hz",
                  _capture.callbacks, _frames, _bpm, _capture.inputPeak, dr, ar);
        }
    } else {
        quiet = 0;
        if (wasSilent) {
            wasSilent = false;
            double dr, ar; [_capture deviceRate:&dr aggregateRate:&ar];
            wvLog("audio back after %.0f s (cbs=%llu frames=%llu) device %.0f Hz, aggregate %.0f Hz",
                  now - silentSince, _capture.callbacks, _frames, dr, ar);
        }
    }
    static int n = 0;
    if ((++n % 20) == 0)
    {
        double dr, ar; [_capture deviceRate:&dr aggregateRate:&ar];
        float icon[3] = {0, 0, 0};                 // mean Dock-icon bar height: lows, mids, highs
        for (int b = 0; b < 32; ++b) icon[b < 8 ? 0 : b < 22 ? 1 : 2] += _bands[b];
        icon[0] /= 8; icon[1] /= 14; icon[2] /= 10;
        wvLog("heartbeat: cbs=%llu frames=%llu peak=%.3f in=%.3f bpm=%.0f hits kick/snare/hat %d/%d/%d bands lo/mid/hi %.2f/%.2f/%.2f icon %.2f/%.2f/%.2f taps=%zu tapRestarts=%ld linkRestarts=%ld device %.0f Hz agg %.0f Hz",
              _capture.callbacks, _frames, _peak, _capture.inputPeak, _bpm, _hits[0], _hits[1], _hits[2],
              _att[0], _att[1], _att[2], icon[0], icon[1], icon[2],
              (size_t)_capture.tapCount, (long)_capture.tapRestarts, (long)_linkRestarts, dr, ar);
        _hits[0] = _hits[1] = _hits[2] = 0;
    }
}

- (NSInteger)tapRestarts { return _capture.tapRestarts; }
- (unsigned long long)renderedFrames { return _frames; }

- (BOOL)start:(NSError **)error
{
    if (_capture.capturing) return YES;
    std::string err;
    if (![_capture start:err]) {
        if (error) {
            *error = [NSError errorWithDomain:@"com.writea.viz.engine" code:1
                userInfo:@{ NSLocalizedDescriptionKey: [NSString stringWithUTF8String:err.c_str()] }];
        }
        fprintf(stderr, "[VizEngine] start failed: %s\n", err.c_str());
        return NO;
    }
    _linkRestarts = 0;
    _frames = 0;
    _capture.renderedFrames = 0;
    [self installScreenObserver];
    fprintf(stderr, "[VizEngine] capturing: %.0f Hz, %d ch\n", _capture.sampleRate, _capture.channels);
    NSString* ver = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    wvLog("capture started: %.0f Hz, %d ch, app %s", _capture.sampleRate, _capture.channels, ver ? ver.UTF8String : "?");

    _lastTick = 0;
    _tickPending = false;
    [self startDisplayLink];
    _supervisor = [NSTimer timerWithTimeInterval:0.5 target:self selector:@selector(supervise)
                                        userInfo:nil repeats:YES];
    _supervisor.tolerance = 0.1;
    [[NSRunLoop mainRunLoop] addTimer:_supervisor forMode:NSRunLoopCommonModes];
    return YES;
}

// vsync thread → main thread. One tick in flight at most: if the main thread
// is busy (a SwiftUI sheet, a preset load) frames are dropped, not queued.
static CVReturn wvDisplayLinkFired(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*,
                                   CVOptionFlags, CVOptionFlags*, void* ctx)
{
    VizEngine* engine = (__bridge VizEngine*)ctx;
    CFTimeInterval now = CACurrentMediaTime();
    if (engine->_cbPrev > 0 && now - engine->_cbPrev > 0.025) engine->_cbLate++;
    engine->_cbPrev = now;
    engine->_cbTime = now;
    [engine scheduleTick];
    return kCVReturnSuccess;
}

- (void)scheduleTick
{
    if (_tickPending.exchange(true)) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        self->_tickPending = false;
        CFTimeInterval now = CACurrentMediaTime();
        self->_cbLatencyMax = std::max(self->_cbLatencyMax, (now - self->_cbTime) * 1000.0);
        // Cap at ~60: on 120 Hz panels every other vsync is skipped (the CPU
        // effect core has no business at 120, classic presets step per frame).
        if (self->_lastTick > 0 && now - self->_lastTick < 0.012) return;
        [self tick:nil];
    });
}

- (void)stop
{
    [_supervisor invalidate]; _supervisor = nil;
    [self stopDisplayLink];
    [self removeScreenObserver];
    if (_capture.capturing) wvLog("capture stopped (cbs=%llu frames=%llu)", _capture.callbacks, _frames);
    [_capture stop];
}

/// SIGUSR1 (and MSCOPES_SNAPSHOT_AFTER): the window, SwiftUI included, and
/// the engine's current frame, as PNGs in the temporary folder. Works with
/// the screen locked, when screencapture can't.
- (void)writeSnapshots
{
        // Window (SwiftUI chrome; the Metal layer's pixels don't come through
        // this path) + the raw engine frame written separately.
        NSView* cv = _view.window.contentView ?: _view;
        NSRect r = cv.bounds;
        NSBitmapImageRep* rep = [cv bitmapImageRepForCachingDisplayInRect:r];
        [cv cacheDisplayInRect:r toBitmapImageRep:rep];
        NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        NSString* dir = NSTemporaryDirectory();
        [png writeToFile:[dir stringByAppendingPathComponent:@"mscopes_window.png"] atomically:YES];

        const viz::Framebuffer& fb = _host.currentSynced();   // may be on-texture
        if (fb.w <= 0 || fb.h <= 0) return;
        NSBitmapImageRep* frep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
            pixelsWide:fb.w pixelsHigh:fb.h bitsPerSample:8 samplesPerPixel:4
            hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
            bytesPerRow:fb.w * 4 bitsPerPixel:32];
        unsigned char* dst = frep.bitmapData;
        for (int y = 0; y < fb.h; ++y) {
            const float* src = fb.at(0, fb.h - 1 - y);          // row 0 is the bottom
            unsigned char* d = dst + (size_t)y * fb.w * 4;
            for (int x = 0; x < fb.w; ++x, src += 4, d += 4) {
                d[0] = (unsigned char)(std::min(1.f, std::max(0.f, src[0])) * 255.f);
                d[1] = (unsigned char)(std::min(1.f, std::max(0.f, src[1])) * 255.f);
                d[2] = (unsigned char)(std::min(1.f, std::max(0.f, src[2])) * 255.f);
                d[3] = 255;
            }
        }
        NSData* fpng = [frep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        [fpng writeToFile:[dir stringByAppendingPathComponent:@"mscopes_frame.png"] atomically:YES];
        fprintf(stderr, "[VizEngine] wrote %s/mscopes_window.png (%lu bytes) + mscopes_frame.png (%dx%d)\n",
                dir.UTF8String, (unsigned long)png.length, fb.w, fb.h);
    }

- (void)tick:(id)sender
{
    (void)sender;

    // Real elapsed time (clamped: a stall or a debugger pause isn't a 5 s frame).
    CFTimeInterval now = CACurrentMediaTime();

    [_capture watchdog:now];                  // re-opens the taps if callbacks stopped
    double dt = _lastTick > 0 ? now - _lastTick : 1.0 / 60.0;
    _lastTick = now;
    _capture.renderedFrames = ++_frames;
    _fxCtx.dt = std::max(1.0 / 240.0, std::min(1.0 / 20.0, dt));

    // Adapt the internal render resolution to the view: half the backing pixel
    // size, clamped. Fixes the "everything always looks blurred" artifact of
    // upscaling a fixed 480x270 buffer to a large window. Hysteresis (>32px)
    // avoids thrashing during live-resize; a resize clears trail history.
    NSSize vb = _view.bounds.size;
    CGFloat scale = _view.window ? _view.window.backingScaleFactor : 2.0;
    // 1 buffer px per point: half the backing size on retina, full size on 1x.
    // High resolution (Picture ▸ Quality): the full backing resolution, up to
    // 2560x1600, for presets that keep within the frame budget there — a
    // preset that doesn't drops back to one pixel per point until the next
    // preset (see the budget check after renderFrame).
    const bool enhance = !_presetIsClassic || _classicEnhanced;
    const bool highRes = _highResolution && enhance && !_highResBlocked;
    CGFloat factor = highRes ? 1.0 : ((scale >= 2.0) ? 0.5 : 1.0);
    const double capW = highRes ? 2560.0 : 1280.0, capH = highRes ? 1600.0 : 800.0;
    int tw = (int)std::min(capW, std::max(480.0, vb.width  * scale * factor));
    int th = (int)std::min(capH, std::max(270.0, vb.height * scale * factor));
    if (std::abs(tw - _host.current().w) > 32 || std::abs(th - _host.current().h) > 32)
        _host.resize(tw, th);
    {   // How lines and dots are drawn this frame (LineMode.h). widthScale:
        // render pixels per point, so a one-pixel line stays one point wide.
        viz::DrawQuality& dq = viz::drawQuality();
        dq.smooth = _smoothLines && enhance;
        // HDR highlights: built-in presets only (classic presets' look
        // depends on clipping at white; they stay exactly as authored).
        dq.hdr = _hdrHighlights && !_presetIsClassic;
        _pic.hdr = dq.hdr;
        dq.widthScale = vb.width > 0 ? std::max(1.f, (float)(_host.current().w / vb.width)) : 1.f;
    }

    // The sensitivity slider also makes the onset detector fire more/less
    // readily (it is ratio-based, so scaling the spectrum alone wouldn't).
    float s = std::max(0.25f, std::min(4.0f, _sensitivity));
    _analyzer.beatDetector().sensitivity = s;

    CFTimeInterval tA = CACurrentMediaTime();
    viz::VizFrame f;
    _analyzer.analyze(f);
    _bpm = f.bpm;
    _hits[0] += f.kickHit; _hits[1] += f.snareHit; _hits[2] += f.hatHit;
    _att[0] = f.bassAtt; _att[1] = f.midAtt; _att[2] = f.trebleAtt;
    {   // MSCOPES_DEBUG_ONSETS=1: every drum candidate and why, to engine.log
        static const bool dbg = getenv("MSCOPES_DEBUG_ONSETS") != nullptr;
        if (dbg) {
            viz::AudioFeatures& fx = _analyzer.features();
            fx.debugCandidates = true;
            for (const auto& c : fx.takeCandidates())
                wvLog("onset %c t=%.3f %s own=%.5f nk=%.5f ns=%.5f nh=%.5f spread=%.2f z=%.1f zS=%.1f zH=%.1f jump=%.1f",
                      c.kind, c.t, c.accepted ? "HIT " : "skip", c.own, c.nk, c.ns, c.nh, c.spread,
                      c.zOwn, c.zS, c.zH, c.lowJump);
        }
    }
    _beatLevel = f.beat ? 1.f : _beatLevel * 0.85f;   // ~200 ms visible flash
    _pic.beat = _beatLevel;
    _pic.bass = std::min(1.f, f.bass * 1.6f);
    _pic.frame++;

    // Apply user sensitivity (SwiftUI-controlled) — real control-path demo.
    if (s != 1.0f) {
        for (int c = 0; c < viz::kMaxChannels; ++c)
            for (int i = 0; i < viz::kSpectrumBins; ++i)
                f.spectrum[c][i] = std::min(1.0f, f.spectrum[c][i] * s);
        f.computeBands();
    }

    _peak = f.peakSpectrum(); _bass = f.bass; _mid = f.mid; _treble = f.treble;
    // Display-ready levels for small meters (the Dock icon): 0..1 bar heights.
    if (f.hasFeatures) {
        // The analyzer's log bands already set their own gain, so every band
        // sits fairly high; squared, a steady band rides mid-height and only
        // its peaks reach the top (plain, the treble looked full all the time).
        static_assert(viz::kBands == 32, "meters show 32 bands");
        for (int b = 0; b < 32; ++b) _bands[b] = f.bands[b] * f.bands[b];
    } else {   // 32 log-spaced bands over bins 1..kSpectrumBins
        const float lo = 1.f, hi = (float)viz::kSpectrumBins;
        for (int b = 0; b < 32; ++b) {
            int i0 = (int)(lo * std::pow(hi / lo, b / 32.f));
            int i1 = (int)(lo * std::pow(hi / lo, (b + 1) / 32.f));
            if (i1 <= i0) i1 = i0 + 1;
            float m = 0.f;
            for (int i = i0; i < i1 && i < viz::kSpectrumBins; ++i) m = std::max(m, f.spectrum[0][i]);
            // Raw spectrum: music rolls off with frequency, so lift the highs
            // and compress, or a small meter shows only the bass.
            const float tilt = 0.7f + 1.1f * (float)b / 31.f;
            _bands[b] = std::min(1.f, std::sqrt(m) * tilt);
        }
    }
    _view->frame = f;

    // Nobody can see the window (minimized, hidden, fully covered, the
    // screen locked or asleep): keep analysing, which the Dock icon and the
    // meters use, but skip drawing, which is nearly all of the energy.
    {
        NSWindow* w = _view.window;
        const BOOL hidden = w && !(w.occlusionState & NSWindowOcclusionStateVisible);
        if (hidden != _windowHidden) {
            _windowHidden = hidden;
            wvLog("%s", hidden ? "window not visible: drawing paused" : "window visible: drawing resumed");
            if (hidden) {
                // Give back what the next frame rebuilds: about 190 MB at
                // 2560x1600 (scratch textures, HDR overflow, previous frame).
                _host.trimMemory();
                viz::gpu::releaseScratch();
            }
        }
        if (hidden) {
            if (gWantWindowSnapshot.exchange(false)) [self writeSnapshots];
            return;
        }
    }

    // Run the effect stack on the analyzed frame.
    _fxCtx.frame++; _fxCtx.time += _fxCtx.dt;
    {   // aspect mode: 0 stretch (classic AVS), 1 fill (crop), 2 fit
        const viz::Framebuffer& fb = _host.current();
        switch (_aspectMode) {
            case 1: _fxCtx.coordSX = _fxCtx.coordSY = std::max(fb.w, fb.h) * 0.5f; break;
            case 2: _fxCtx.coordSX = _fxCtx.coordSY = std::min(fb.w, fb.h) * 0.5f; break;
            default: _fxCtx.coordSX = _fxCtx.coordSY = 0.f; break;
        }
    }
    CFTimeInterval tR = CACurrentMediaTime();
    _host.renderFrame(f, _fxCtx);
    CFTimeInterval tE = CACurrentMediaTime();
    _pic.overflow = _pic.hdr ? &_host.overflow() : nullptr;

    [_view presentFrame];

    // Frame budget at high resolution: render + present averaging over 11 ms
    // for a second and a half means this preset can't hold 60 fps up there.
    {
        double ms = (tE - tR) * 1000.0 + _view->blitMs;
        _renderMsAvg = _renderMsAvg * 0.95 + ms * 0.05;
        if (highRes && _renderMsAvg > 11.0) {
            if (++_overBudgetFrames > 90) {
                _highResBlocked = YES;
                _overBudgetFrames = 0;
                wvLog("high resolution off for this preset: %.1f ms/frame at %dx%d",
                      _renderMsAvg, _host.current().w, _host.current().h);
            }
        } else {
            _overBudgetFrames = 0;
        }
    }

    // Profiling accumulators (ms), reported once a second. The tick-interval
    // histogram tells whether the display link is actually firing per vsync.
    static double accAnalyze = 0, accRender = 0, accBlit = 0, maxTick = 0;
    static int dtFast = 0, dtOne = 0, dtTwo = 0, dtMore = 0;
    accAnalyze += (tR - tA) * 1000.0; accRender += (tE - tR) * 1000.0; accBlit += _view->blitMs;
    maxTick = std::max(maxTick, (CACurrentMediaTime() - now) * 1000.0);
    if (dt < 0.012) dtFast++; else if (dt < 0.020) dtOne++; else if (dt < 0.037) dtTwo++; else dtMore++;

    static int n = 0;
    static CFTimeInterval lastLog = 0;
    if ((++n % 60) == 0) {
        CFTimeInterval tNow = CACurrentMediaTime();
        double fps = lastLog > 0 ? 60.0 / (tNow - lastLog) : 0;
        lastLog = tNow;
        double pu = 0, pd = 0, pg = 0;
        viz::gpu::lastPresentTimes(&pu, &pd, &pg);
        fprintf(stderr, "[VizEngine] cbs=%llu peak=%.3f bass=%.3f bassRms=%.4f bpm=%.1f res=%dx%d fps=%.0f "
                        "| ms/frame analyze=%.2f render=%.2f blit=%.2f (up=%.2f drw=%.2f gpu=%.2f) tickMax=%.1f "
                        "| dt<12:%d 1v:%d 2v:%d >2v:%d | cbLate=%d cbLatMax=%.1f | screen %ld Hz\n",
                _capture.callbacks, _peak, _bass, _analyzer.lastBassRms(), _bpm,
                _host.current().w, _host.current().h, fps,
                accAnalyze / 60.0, accRender / 60.0, accBlit / 60.0, pu, pd, pg, maxTick,
                dtFast, dtOne, dtTwo, dtMore, _cbLate, _cbLatencyMax,
                (long)(_view.window.screen ? _view.window.screen.maximumFramesPerSecond : 0));
        accAnalyze = accRender = accBlit = maxTick = 0;
        dtFast = dtOne = dtTwo = dtMore = 0;
        _cbLate = 0; _cbLatencyMax = 0;
        const auto& prof = _host.lastProfile();
        if (!prof.empty()) {
            std::string s;
            for (size_t i = 0; i < prof.size() && i < 4; ++i) {
                char b[96];
                snprintf(b, sizeof b, "%s%s=%.2f", i ? ", " : "", prof[i].first.c_str(), prof[i].second);
                s += b;
            }
            fprintf(stderr, "[VizEngine]   top effects (ms): %s\n", s.c_str());
        }
    }

    if (gWantWindowSnapshot.exchange(false)) [self writeSnapshots];
}

@end
