//
// VizEngine.mm — Objective-C++ implementation bridging the C++ core to Swift.
//
// Compiled under ARC (Xcode default). The C++ members (Analyzer, tap pointer)
// are constructed/destructed with the ObjC object. AppKit drawing is ARC-safe.
//
#import "VizEngine.h"
#import <QuartzCore/QuartzCore.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include "VizFrame.h"
#include "GpuFx.h"
#import  "Analyzer.h"
#import  "SystemAudioTap.h"
#include <os/log.h>
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
#import  "FramebufferBlit.h"

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
//  Private drawing view (kept out of the Swift-visible header on purpose).
//
//  Metal-backed: the view hosts a CAMetalLayer and gpu::present() draws the
//  engine's framebuffer straight into its drawable each tick. The old path —
//  float→8-bit conversion, NSBitmapImageRep drawInRect, and AppKit's
//  backing-store commit of a full-window view — capped the app at ~45 fps
//  even when the effects themselves took under 10 ms. Kept as the fallback
//  (drawRect) for machines without Metal.
// ---------------------------------------------------------------------------
@interface WVRenderView : NSView
{
@public
    viz::VizFrame frame;             // latest audio frame (for the HUD readout)
    const viz::Framebuffer* fb;      // effect-host output to present (owned by engine)
    BOOL          showHUD;
    double        blitMs;            // last present/blit cost (profiling)
@private
    NSBitmapImageRep* _blitRep;      // CPU fallback: reused across frames
    BOOL              _metal;
    CATextLayer*      _hud;
    int               _hudTick;
}
- (void)presentFrame;
@end

@implementation WVRenderView
- (instancetype)initWithFrame:(NSRect)f
{
    if ((self = [super initWithFrame:f])) {
        _metal = viz::gpu::available();
        if (_metal) {
            self.wantsLayer = YES;
            self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
        }
    }
    return self;
}

- (BOOL)isOpaque { return YES; }
- (BOOL)wantsUpdateLayer { return _metal; }     // Metal path never uses drawRect
- (void)updateLayer {}                          // presentFrame draws instead

- (CALayer*)makeBackingLayer
{
    if (!_metal) return [super makeBackingLayer];
    CAMetalLayer* l = [CAMetalLayer layer];
    l.device = (__bridge id<MTLDevice>)viz::gpu::metalDevice();
    l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    l.framebufferOnly = YES;
    l.opaque = YES;
    l.presentsWithTransaction = NO;
    l.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    return l;
}

- (void)syncDrawableSize
{
    if (!_metal || !self.layer) return;
    CAMetalLayer* l = (CAMetalLayer*)self.layer;
    CGFloat scale = self.window ? self.window.backingScaleFactor : 2.0;
    NSSize b = self.bounds.size;
    l.contentsScale = scale;
    l.drawableSize = CGSizeMake(std::max(1.0, b.width * scale), std::max(1.0, b.height * scale));
    _hud.contentsScale = scale;
}
- (void)setFrameSize:(NSSize)s { [super setFrameSize:s]; [self syncDrawableSize]; }
- (void)viewDidMoveToWindow { [super viewDidMoveToWindow]; [self syncDrawableSize]; }
- (void)viewDidChangeBackingProperties { [super viewDidChangeBackingProperties]; [self syncDrawableSize]; }

- (NSString*)hudText
{
    return [NSString stringWithFormat:@"peak %.2f   bass %.2f   mid %.2f   treble %.2f   bpm %.0f",
            frame.peakSpectrum(), frame.bass, frame.mid, frame.treble, frame.bpm];
}

- (void)updateHUD
{
    if (!showHUD) { _hud.hidden = YES; return; }
    if (!_hud) {
        _hud = [CATextLayer layer];
        _hud.font = (__bridge CFTypeRef)[NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        _hud.fontSize = 11;
        _hud.foregroundColor = CGColorCreateGenericGray(1.0, 0.85);
        _hud.anchorPoint = CGPointMake(0, 1);
        _hud.contentsScale = self.layer.contentsScale;
        [self.layer addSublayer:_hud];
    }
    _hud.hidden = NO;
    if ((_hudTick++ % 6) == 0) {                 // 10 Hz is plenty for a readout
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        _hud.string = [self hudText];
        _hud.frame = CGRectMake(10, self.bounds.size.height - 8, self.bounds.size.width - 20, 16);
        [CATransaction commit];
    }
}

- (void)presentFrame
{
    if (!fb || fb->w == 0) return;
    CFTimeInterval t0 = CACurrentMediaTime();
    if (_metal) {
        if (!self.layer) return;
        viz::gpu::present(*fb, (__bridge void*)self.layer);
        [self updateHUD];
    } else {
        [self setNeedsDisplay:YES];
    }
    blitMs = (CACurrentMediaTime() - t0) * 1000.0;
}

// CPU fallback (no Metal device).
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSRect b = self.bounds;

    [[NSColor blackColor] setFill];
    NSRectFill(b);

    if (fb && fb->w > 0) {
        CFTimeInterval t0 = CACurrentMediaTime();
        viz::BlitFramebuffer(*fb, b, &_blitRep);
        blitMs = (CACurrentMediaTime() - t0) * 1000.0;
    }

    if (showHUD) {
        NSDictionary* a = @{
            NSForegroundColorAttributeName: [NSColor colorWithCalibratedWhite:1 alpha:0.85],
            NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular],
        };
        [[self hudText] drawAtPoint:NSMakePoint(10, b.size.height - 22) withAttributes:a];
    }
}
@end

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
    viz::Analyzer         _analyzer;
    // One tap per process producing output (a tap that mixes several
    // processes drops out on macOS 26 — see SystemAudioTap.mm); each feeds
    // its own analyzer slot and is watched and re-opened on its own.
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
    std::vector<std::unique_ptr<TapSlot>> _slots;
    WVRenderView*         _view;
    // Tap supervision: a live tap stops delivering when the default output
    // device changes, its sample rate changes, or the Mac sleeps. The tick
    // watches the callback counter and re-opens the tap when it stalls; the
    // device-change and wake listeners do it eagerly.
    std::atomic<float>    _inPeak;         // raw tap input level (pre-analysis)
    unsigned long long    _cbsSeen;
    CFTimeInterval        _cbsSeenAt;
    CFTimeInterval        _lastTapRestart;
    NSInteger             _tapRestarts;
    NSInteger             _linkRestarts;
    unsigned long long    _frames;         // ticks rendered since capture started
    NSTimer*              _supervisor;     // 1 Hz: display-link watchdog + heartbeat
    id                    _screenObserver;
    BOOL                  _listenersInstalled;
    id                    _wakeObserver;
    AudioObjectPropertyListenerBlock _defaultDeviceListener;
    unsigned              _procGen;        // coalesces bursts of process events
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
    std::atomic<unsigned long long> _cbs;

    viz::EffectHost       _host;
    viz::EffectContext    _fxCtx;
    int                   _aspectMode;   // 0 stretch, 1 fill, 2 fit
    NSInteger             _currentPreset;

    double _sampleRate;
    int    _channels;
    BOOL   _capturing;
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
        _aspectMode = 1;               // fill: shapes stay round fullscreen
        _cbs = 0;
        gEngineForSignals = self;
        signal(SIGUSR1, wvOnSIGUSR1);
        signal(SIGUSR2, wvOnSIGUSR2);

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
- (BOOL)isCapturing    { return _capturing; }
- (double)sampleRate   { return _sampleRate; }
- (NSInteger)channels  { return _channels; }
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
- (unsigned long long)audioCallbacks { return _cbs.load(); }

- (void)setShowHUD:(BOOL)showHUD { _showHUD = showHUD; _view->showHUD = showHUD; }

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
    } else {
        viz::loadAvsPresetFile(_host, std::string(path.UTF8String), rep, true);
    }
    return safeNS(rep.summary());
}

- (void)setAspectMode:(NSInteger)mode { _aspectMode = (int)mode; }
- (NSInteger)aspectMode { return _aspectMode; }

- (void)setGlobalReg:(NSInteger)index value:(float)value
{
    *viz::eel::VM::globalRegSlot((int)index) = value;
}
- (float)globalReg:(NSInteger)index
{
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
    return ok ? [NSString stringWithFormat:@"loaded, %zu effects", _host.count()]
              : safeNS(err);
}
- (void)moveEffectAt:(NSInteger)from to:(NSInteger)to
{
    if (from < 0 || to < 0) return;
    _host.move((size_t)from, (size_t)to);
}

static os_log_t wvEngineLog(void)
{
    static os_log_t l = os_log_create("com.writea.viz", "engine");
    return l;
}

// A plain log file next to the system ones (~/Library/Logs/MScopes/engine.log):
// capture start/stop, tap re-opens and why, display-link restarts, and a
// heartbeat every 10 s with the counters — so a freeze that happened while
// nobody was looking can still be explained afterwards. Truncated at 2 MB.
static void wvLog(const char* fmt, ...)
{
    static FILE* f = nullptr;
    if (!f) {
        NSString* dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/MScopes"];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        NSString* path = [dir stringByAppendingPathComponent:@"engine.log"];
        NSDictionary* attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
        f = fopen(path.fileSystemRepresentation, ([attrs fileSize] > 2 * 1024 * 1024) ? "w" : "a");
        if (!f) return;
        setvbuf(f, nullptr, _IOLBF, 0);
    }
    char ts[32];
    time_t t = time(nullptr);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(f, "%s ", ts);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
}

static std::string wvProcName(AudioObjectID proc)
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

static bool wvProcessRunningOutput(AudioObjectID proc)
{
    UInt32 running = 0, sz = sizeof(running);
    AudioObjectPropertyAddress ra = { kAudioProcessPropertyIsRunningOutput,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    return AudioObjectGetPropertyData(proc, &ra, 0, nullptr, &sz, &running) == noErr && running;
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
    viz::Analyzer* an = &_analyzer;
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
    if (!tap->start(err, procs)) return NO;
    slot->tap = std::move(tap);
    slot->lastRestart = CACurrentMediaTime();
    slot->quietChecks = 0;
    slot->peak.store(0.f);
    slot->hadSignal.store(false);
    if (_sampleRate <= 0) { _sampleRate = slot->tap->sampleRate(); _channels = slot->tap->channels(); }
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
            wvLog("tap closed: %s stopped playing (%s)", (*it)->name.c_str(), why);
            it = _slots.erase(it);
        } else ++it;
    }
    std::string err;
    for (AudioObjectID p : wanted) {
        bool have = false;
        for (auto& sl : _slots) if (sl->proc == p) { have = true; break; }
        if (have) continue;
        auto sl = std::make_unique<TapSlot>();
        sl->proc = p;
        sl->name = wvProcName(p);
        sl->index = [self freeSlotIndex];
        if (sl->index < 0) break;
        if ([self openSlot:sl.get() err:err]) {
            wvLog("tap opened: %s (%s) -> slot %d, %.0f Hz, %d ch", sl->name.c_str(), why,
                  sl->index, sl->tap->sampleRate(), sl->tap->channels());
            _slots.push_back(std::move(sl));
        } else {
            wvLog("tap open FAILED for %s: %s", sl->name.c_str(), err.c_str());
        }
    }
    _cbsSeen   = _cbs.load();
    _cbsSeenAt = CACurrentMediaTime();
    return (NSInteger)_slots.size();
}

- (void)debugStallTap
{
    for (auto& sl : _slots) if (sl->tap) sl->tap->pauseForTest();
    wvLog("SIGUSR2: tap IO stopped on %zu tap(s) for the watchdog test", _slots.size());
    fprintf(stderr, "[VizEngine] SIGUSR2: tap IO stopped for the watchdog test\n");
}

/// Re-open every tap (device change, wake, all callbacks stopped). Main
/// thread only. A failure is logged and left to the watchdog.
- (void)restartTapBecause:(const char*)why
{
    if (!_capturing) return;
    _lastTapRestart = CACurrentMediaTime();
    wvLog("tap re-open requested: %s (cbs=%llu, frames=%llu, taps=%zu)", why, _cbs.load(), _frames, _slots.size());
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
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{ [self restartTapBecause:why]; });
}

- (void)installTapListeners
{
    if (_listenersInstalled) return;
    _listenersInstalled = YES;
    // Default output device changed (headphones, AirPods, a display, …).
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    __weak VizEngine* weakSelf = self;
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
        VizEngine* me = weakSelf; if (!me) return;
        unsigned gen = ++me->_procGen;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.2 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            VizEngine* me2 = weakSelf; if (!me2 || gen != me2->_procGen || !me2->_capturing) return;
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
    // Displays changed (one slept, was unplugged, changed mode): the
    // CVDisplayLink may keep running or may not — rebuild it to be sure.
    _screenObserver = [[NSNotificationCenter defaultCenter]
        addObserverForName:NSApplicationDidChangeScreenParametersNotification object:nil
        queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification*) {
            wvLog("event: screen parameters changed");
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                           dispatch_get_main_queue(), ^{ [weakSelf restartDisplayLinkBecause:"screen parameters changed"]; });
        }];
}

- (void)removeTapListeners
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
    if (_screenObserver) {
        [[NSNotificationCenter defaultCenter] removeObserver:_screenObserver];
        _screenObserver = nil;
    }
}

// Sample rate of the default output device and of our aggregate, for the
// log: Apple Music switches the output device's rate per track (44.1/48/96 k
// for lossless), and a tap can go quiet while CoreAudio reconfigures.
static void wvRates(double* deviceRate, double* aggRate, AudioObjectID agg)
{
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
    if (!_capturing) return;
    [self stopDisplayLink];
    [self startDisplayLink];
    _linkRestarts++;
    _lastTick = CACurrentMediaTime();
    wvLog("display link rebuilt (%s), restart #%ld", why, (long)_linkRestarts);
    fprintf(stderr, "[VizEngine] display link rebuilt (%s)\n", why);
}

/// Twice a second: rebuild the display link if ticks stopped, re-open a
/// silent tap, and write the heartbeat every 10 s.
- (void)supervise
{
    if (!_capturing) return;
    CFTimeInterval now = CACurrentMediaTime();
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
        if (!wvProcessRunningOutput(sl->proc)) continue;
        if (sl->hadSignal.load()) {
            if (sl->quietChecks >= 3 && now - sl->lastRestart > 1.9) [self reopenSlot:sl.get()];
        } else {
            double wait = 10.0 * (double)(1 << std::min(sl->blindRetries, 5));
            if (now - sl->lastRestart > wait) { sl->blindRetries++; [self reopenSlot:sl.get()]; }
        }
    }
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
            double dr, ar; wvRates(&dr, &ar, (!_slots.empty() && _slots[0]->tap) ? _slots[0]->tap->aggregateID() : kAudioObjectUnknown);
            wvLog("audio silent (cbs=%llu frames=%llu bpm=%.0f input peak %.4f) device %.0f Hz, aggregate %.0f Hz",
                  _cbs.load(), _frames, _bpm, _inPeak.load(), dr, ar);
        }
    } else {
        quiet = 0;
        if (wasSilent) {
            wasSilent = false;
            double dr, ar; wvRates(&dr, &ar, (!_slots.empty() && _slots[0]->tap) ? _slots[0]->tap->aggregateID() : kAudioObjectUnknown);
            wvLog("audio back after %.0f s (cbs=%llu frames=%llu) device %.0f Hz, aggregate %.0f Hz",
                  now - silentSince, _cbs.load(), _frames, dr, ar);
        }
    }
    static int n = 0;
    if ((++n % 20) == 0)
    {
        double dr, ar; wvRates(&dr, &ar, (!_slots.empty() && _slots[0]->tap) ? _slots[0]->tap->aggregateID() : kAudioObjectUnknown);
        wvLog("heartbeat: cbs=%llu frames=%llu peak=%.3f in=%.3f bpm=%.0f taps=%zu tapRestarts=%ld linkRestarts=%ld device %.0f Hz agg %.0f Hz",
              _cbs.load(), _frames, _peak, _inPeak.load(), _bpm, _slots.size(), (long)_tapRestarts, (long)_linkRestarts, dr, ar);
    }
}

- (NSInteger)tapRestarts { return _tapRestarts; }
- (unsigned long long)renderedFrames { return _frames; }

- (BOOL)start:(NSError **)error
{
    if (_capturing) return YES;

    _sampleRate = 0; _channels = 0;
    NSInteger taps = [self reconcileTapsBecause:"capture start"];
    if (taps == 0) {
        // Nothing is playing yet; taps open as soon as something does. Still
        // make sure a tap CAN be made (the system prompts for permission on
        // the first one): try a throwaway global tap and surface its error.
        std::string err;
        viz::SystemAudioTap probe([](const float*, int, int, double) {});
        std::vector<AudioObjectID> none;
        if (!probe.start(err, none)) {
            if (error) {
                *error = [NSError errorWithDomain:@"com.writea.viz.engine" code:1
                    userInfo:@{ NSLocalizedDescriptionKey:
                        [NSString stringWithUTF8String:err.c_str()] }];
            }
            fprintf(stderr, "[VizEngine] start failed: %s\n", err.c_str());
            return NO;
        }
        _sampleRate = probe.sampleRate(); _channels = probe.channels();
        probe.stop();
    }

    _capturing   = YES;
    _tapRestarts = 0;
    _linkRestarts = 0;
    _frames = 0;
    [self installTapListeners];
    fprintf(stderr, "[VizEngine] capturing: %.0f Hz, %d ch\n", _sampleRate, _channels);
    NSString* ver = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    wvLog("capture started: %.0f Hz, %d ch, app %s", _sampleRate, _channels, ver ? ver.UTF8String : "?");

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
    [self removeTapListeners];
    if (_capturing) wvLog("capture stopped (cbs=%llu frames=%llu)", _cbs.load(), _frames);
    _slots.clear();
    _capturing = NO;
}

- (void)tick:(id)sender
{
    (void)sender;

    // Real elapsed time (clamped: a stall or a debugger pause isn't a 5 s frame).
    CFTimeInterval now = CACurrentMediaTime();

    // Stall watchdog: the tap delivers callbacks continuously, silence
    // included, so none for 3 s means it died — re-open it (at most every 5 s).
    {
        unsigned long long c = _cbs.load();
        if (c != _cbsSeen) { _cbsSeen = c; _cbsSeenAt = now; }
        else if (_capturing && !_slots.empty() && _cbsSeenAt > 0 && now - _cbsSeenAt > 3.0 && now - _lastTapRestart > 5.0)
            [self restartTapBecause:"no audio callbacks for 3 s"];
    }
    double dt = _lastTick > 0 ? now - _lastTick : 1.0 / 60.0;
    _lastTick = now;
    _frames++;
    {   // raw input level: the loudest tap
        float m = 0.f;
        for (auto& sl : _slots) m = std::max(m, sl->peak.load());
        _inPeak.store(m);
    }
    _fxCtx.dt = std::max(1.0 / 240.0, std::min(1.0 / 20.0, dt));

    // Adapt the internal render resolution to the view: half the backing pixel
    // size, clamped. Fixes the "everything always looks blurred" artifact of
    // upscaling a fixed 480x270 buffer to a large window. Hysteresis (>32px)
    // avoids thrashing during live-resize; a resize clears trail history.
    NSSize vb = _view.bounds.size;
    CGFloat scale = _view.window ? _view.window.backingScaleFactor : 2.0;
    // 1 buffer px per point: half the backing size on retina, full size on 1x.
    CGFloat factor = (scale >= 2.0) ? 0.5 : 1.0;
    int tw = (int)std::min(1280.0, std::max(480.0, vb.width  * scale * factor));
    int th = (int)std::min(800.0,  std::max(270.0, vb.height * scale * factor));
    if (std::abs(tw - _host.current().w) > 32 || std::abs(th - _host.current().h) > 32)
        _host.resize(tw, th);

    // The sensitivity slider also makes the onset detector fire more/less
    // readily (it is ratio-based, so scaling the spectrum alone wouldn't).
    float s = std::max(0.25f, std::min(4.0f, _sensitivity));
    _analyzer.beatDetector().sensitivity = s;

    CFTimeInterval tA = CACurrentMediaTime();
    viz::VizFrame f;
    _analyzer.analyze(f);
    _bpm = f.bpm;
    _beatLevel = f.beat ? 1.f : _beatLevel * 0.85f;   // ~200 ms visible flash

    // Apply user sensitivity (SwiftUI-controlled) — real control-path demo.
    if (s != 1.0f) {
        for (int c = 0; c < viz::kMaxChannels; ++c)
            for (int i = 0; i < viz::kSpectrumBins; ++i)
                f.spectrum[c][i] = std::min(1.0f, f.spectrum[c][i] * s);
        f.computeBands();
    }

    _peak = f.peakSpectrum(); _bass = f.bass; _mid = f.mid; _treble = f.treble;
    {   // 32 log-spaced bands over bins 1..kSpectrumBins for small meters
        const float lo = 1.f, hi = (float)viz::kSpectrumBins;
        for (int b = 0; b < 32; ++b) {
            int i0 = (int)(lo * std::pow(hi / lo, b / 32.f));
            int i1 = (int)(lo * std::pow(hi / lo, (b + 1) / 32.f));
            if (i1 <= i0) i1 = i0 + 1;
            float m = 0.f;
            for (int i = i0; i < i1 && i < viz::kSpectrumBins; ++i) m = std::max(m, f.spectrum[0][i]);
            _bands[b] = std::min(1.f, m);
        }
    }
    _view->frame = f;

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

    [_view presentFrame];

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
                _cbs.load(), _peak, _bass, _analyzer.lastBassRms(), _bpm,
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

    if (gWantWindowSnapshot.exchange(false)) {
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
}

@end
