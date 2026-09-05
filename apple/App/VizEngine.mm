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
    viz::SystemAudioTap*  _tap;
    WVRenderView*         _view;
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
        signal(SIGUSR1, wvOnSIGUSR1);

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

- (BOOL)start:(NSError **)error
{
    if (_capturing) return YES;

    viz::Analyzer* an = &_analyzer;
    std::atomic<unsigned long long>* cbs = &_cbs;
    _tap = new viz::SystemAudioTap(
        [an, cbs](const float* interleaved, int frames, int channels, double sr) {
            (void)sr;
            an->push(interleaved, frames, channels);
            cbs->fetch_add(1);
        });

    std::string err;
    if (!_tap->start(err)) {
        if (error) {
            *error = [NSError errorWithDomain:@"com.writea.viz.engine" code:1
                userInfo:@{ NSLocalizedDescriptionKey:
                    [NSString stringWithUTF8String:err.c_str()] }];
        }
        fprintf(stderr, "[VizEngine] start failed: %s\n", err.c_str());
        delete _tap; _tap = nullptr;
        return NO;
    }

    _sampleRate = _tap->sampleRate();
    _channels   = _tap->channels();
    _capturing  = YES;
    fprintf(stderr, "[VizEngine] capturing: %.0f Hz, %d ch\n", _sampleRate, _channels);

    _lastTick = 0;
    _tickPending = false;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // CVDisplayLink: see ivar note
    if (CVDisplayLinkCreateWithActiveCGDisplays(&_cvLink) == kCVReturnSuccess) {
        CVDisplayLinkSetOutputCallback(_cvLink, wvDisplayLinkFired, (__bridge void*)self);
        CVDisplayLinkStart(_cvLink);
    } else {
        fprintf(stderr, "[VizEngine] CVDisplayLink unavailable; falling back to a 60 Hz timer\n");
        NSTimer* t = [NSTimer timerWithTimeInterval:1.0/60.0 target:self
                                           selector:@selector(tick:) userInfo:nil repeats:YES];
        [[NSRunLoop mainRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
        _fallbackTimer = t;
    }
#pragma clang diagnostic pop
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
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (_cvLink) { CVDisplayLinkStop(_cvLink); CVDisplayLinkRelease(_cvLink); _cvLink = nullptr; }
#pragma clang diagnostic pop
    [_fallbackTimer invalidate]; _fallbackTimer = nil;
    if (_tap) { _tap->stop(); delete _tap; _tap = nullptr; }
    _capturing = NO;
}

- (void)tick:(id)sender
{
    (void)sender;

    // Real elapsed time (clamped: a stall or a debugger pause isn't a 5 s frame).
    CFTimeInterval now = CACurrentMediaTime();
    double dt = _lastTick > 0 ? now - _lastTick : 1.0 / 60.0;
    _lastTick = now;
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
