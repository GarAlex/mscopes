//
// SpectrumBarPlugin.mm
//
// A minimal iTunes/Music Visual Plugin used as a de-risking *spike*.
//
// Purpose (see apple/MusicPlugin/README.md):
//   1. Confirm a third-party visual plugin still LOADS into modern, sandboxed
//      Music.app on macOS Tahoe (macOS 26) at the ~/Library/iTunes install path.
//   2. Confirm Music still DELIVERS audio via kVisualPluginPulseMessage
//      (RenderVisualData: 512-entry spectrum + waveform, up to 2 channels).
//   3. Confirm we can DRAW into the NSView Music hands us on Activate.
//
// It draws live spectrum bars + a waveform trace, plus an always-on diagnostics
// HUD (pulse/draw counters, channel count, peak spectrum value). If Music loads
// the plugin but sends no pulse data, a red "NO PULSE DATA" banner appears — so
// the central unknown is answerable just by watching the screen.
//
// This file is original work (MIT). It links against the vendored Apple iTunes
// Visual SDK headers in third_party/itunes-visual-sdk (Apple sample-code license).
// It contains no projectM (LGPL) code.
//
// Compiled as Objective-C++ with manual reference counting (no ARC), matching
// the C-struct-holds-ObjC-pointer style of the Apple SDK sample.
//

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <os/log.h>
#include <string.h>
#include <math.h>
#include <new>

#include "iTunesVisualAPI.h"   // pulls in iTunesAPI.h
#include "VizFrame.h"          // shared core data model
#include "BeatDetector.h"      // shared onset/BPM detector
#import  "Renderer.h"          // shared core renderer

// Namespaced ObjC class name to avoid load-time collisions with other visualizers.
#define WVSpikeView  MScopes_PluginView

// ---------------------------------------------------------------------------
//  Plugin identity
// ---------------------------------------------------------------------------
#define kPluginDisplayName   CFSTR("MScopes")
#define kPluginCreator       'WVsp'         // OSType identifying this plugin
#define kPluginPulseRateHz   120            // request data up to 120x/sec
#define kNoDataBannerAfterSec 2.0           // show banner if no pulse this long while active

static os_log_t gLog(void)
{
    static os_log_t log;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ log = os_log_create("com.writea.viz.musicplugin", "plugin"); });
    return log;
}

@class WVSpikeView;

// ---------------------------------------------------------------------------
//  Per-instance plugin state (malloc'd; holds unretained/retained ObjC ptrs)
// ---------------------------------------------------------------------------
typedef struct SpikeData {
    void*           appCookie;
    ITAppProcPtr    appProc;

    NSView*         destView;      // container view Music hands us (not retained)
    WVSpikeView*    subview;       // our drawing view (retained)

    viz::VizFrame   frame;         // most recent analyzed frame (shared model)
    viz::BeatDetector beat;        // onset/BPM detector fed by each pulse
    UInt32          renderTimeStampID;

    uint64_t        pulseCount;    // total pulse messages seen
    uint64_t        drawCount;     // total drawRect calls
    UInt8           peakSpectrum;  // peak spectrum value in last pulse (0-255)
    bool            haveData;      // received at least one pulse
    bool            playing;       // Music reports playback active
    CFTimeInterval  lastPulseTime; // CACurrentMediaTime of last pulse
    CFTimeInterval  activateTime;  // when Activate happened
} SpikeData;

// ---------------------------------------------------------------------------
//  Drawing view
// ---------------------------------------------------------------------------
@interface WVSpikeView : NSView
{
    SpikeData* _data;
    NSTimer*   _timer;
}
@property(nonatomic, assign) SpikeData* data;
- (void)stopAnimating;
@end

@implementation WVSpikeView

@synthesize data = _data;

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = YES;                 // layer-backed for smooth compositing
        self.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        // Drive our own animation clock (~60fps). Pulse arrival is independent.
        _timer = [NSTimer timerWithTimeInterval:1.0/60.0
                                         target:self
                                       selector:@selector(tick:)
                                       userInfo:nil
                                        repeats:YES];
        [[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
    }
    return self;
}

// The run loop retains the timer and the timer retains us (its target), so a
// plain -release would never dealloc us and the timer would keep firing on an
// orphaned view. Deactivate calls this first to break the cycle.
- (void)stopAnimating
{
    [_timer invalidate];
    _timer = nil;
}

- (void)dealloc
{
    [self stopAnimating];
    [super dealloc];
}

- (BOOL)isOpaque { return YES; }             // opaque: Music won't waste time drawing behind us

- (void)tick:(NSTimer*)t
{
    (void)t;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirtyRect
{
    (void)dirtyRect;
    SpikeData* d = _data;
    const NSRect b = self.bounds;
    const CGFloat W = b.size.width, H = b.size.height;

    if (!d) {
        [[NSColor blackColor] setFill];
        NSRectFill(b);
        return;
    }
    d->drawCount++;
    d->frame.frameIndex = d->drawCount;    // heartbeat animates per draw

    // shared-core visual: background + spectrum bars + waveform + heartbeat
    viz::DrawVisual(d->frame, b);

    // --- diagnostics HUD ---
    CFTimeInterval now = CACurrentMediaTime();
    CFTimeInterval sincePulse = d->haveData ? (now - d->lastPulseTime) : -1;
    NSString* hud = [NSString stringWithFormat:
        @"MScopes\npulses: %llu   draws: %llu\nts: %u   ch: %d   peak: %d   playing: %d\nsince last pulse: %@",
        (unsigned long long)d->pulseCount,
        (unsigned long long)d->drawCount,
        (unsigned)d->renderTimeStampID,
        (int)d->frame.numSpectrumChannels,
        (int)d->peakSpectrum,
        (int)d->playing,
        d->haveData ? [NSString stringWithFormat:@"%.2fs", sincePulse] : @"(never)"];
    NSDictionary* attrs = @{
        NSForegroundColorAttributeName: [NSColor colorWithCalibratedWhite:1.0 alpha:0.9],
        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular],
    };
    [hud drawAtPoint:NSMakePoint(12, H - 84) withAttributes:attrs];

    // --- "NO PULSE DATA" banner: the key Tahoe signal ---
    bool stale = (!d->haveData && (now - d->activateTime) > kNoDataBannerAfterSec) ||
                 (d->haveData && sincePulse > kNoDataBannerAfterSec);
    if (stale) {
        NSString* msg = d->haveData ? @"⚠︎ PULSE DATA STOPPED" : @"⚠︎ NO PULSE DATA FROM MUSIC";
        NSDictionary* ba = @{
            NSForegroundColorAttributeName: [NSColor colorWithCalibratedRed:1 green:0.3 blue:0.3 alpha:1],
            NSFontAttributeName: [NSFont boldSystemFontOfSize:22],
        };
        NSSize sz = [msg sizeWithAttributes:ba];
        [msg drawAtPoint:NSMakePoint((W - sz.width) * 0.5, H * 0.5) withAttributes:ba];
    }
}
@end

// ---------------------------------------------------------------------------
//  Registration helpers
// ---------------------------------------------------------------------------
static void GetVisualName(ITUniStr255 name)
{
    CFIndex length = CFStringGetLength(kPluginDisplayName);
    if (length > 255) length = 255;
    name[0] = (UniChar)length;
    CFStringGetCharacters(kPluginDisplayName, CFRangeMake(0, length), &name[1]);
}

static OptionBits GetVisualOptions(void)
{
    // 2D CoreGraphics subview → NOT kVisualUsesOnly3D.
    return kVisualUsesSubview | kVisualWantsIdleMessages;
}

// ---------------------------------------------------------------------------
//  Per-message handler
// ---------------------------------------------------------------------------
static OSStatus VisualPluginHandler(OSType message, VisualPluginMessageInfo* messageInfo, void* refCon)
{
    SpikeData* d = (SpikeData*)refCon;
    OSStatus status = noErr;

    switch (message) {
        case kVisualPluginInitMessage: {
            d = new (std::nothrow) SpikeData();   // value-inits (constructs VizFrame member)
            if (!d) { status = memFullErr; break; }
            d->appCookie = messageInfo->u.initMessage.appCookie;
            d->appProc   = messageInfo->u.initMessage.appProc;
            messageInfo->u.initMessage.refCon = (void*)d;
            os_log(gLog(), "INIT: plugin instance created %p", (void*)d);
            break;
        }

        case kVisualPluginCleanupMessage: {
            os_log(gLog(), "CLEANUP: instance %p (pulses=%llu draws=%llu)",
                   (void*)d, d ? (unsigned long long)d->pulseCount : 0,
                   d ? (unsigned long long)d->drawCount : 0);
            delete d;   // safe on nullptr
            break;
        }

        case kVisualPluginEnableMessage:
        case kVisualPluginDisableMessage:
        case kVisualPluginIdleMessage:
            break;

        case kVisualPluginActivateMessage: {
            if (!d) { status = paramErr; break; }
            NSView* container = (NSView*)messageInfo->u.activateMessage.view;  // NSOpenGLView* -> NSView*
            d->destView = container;
            d->activateTime = CACurrentMediaTime();
            d->haveData = false;

            WVSpikeView* v = [[WVSpikeView alloc] initWithFrame:[container bounds]];
            v.data = d;
            [v setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
            [container addSubview:v];
            d->subview = v;   // retained (alloc)
            os_log(gLog(), "ACTIVATE: view=%p bounds=%.0fx%.0f",
                   (void*)container, [container bounds].size.width, [container bounds].size.height);
            break;
        }

        case kVisualPluginDeactivateMessage: {
            os_log(gLog(), "DEACTIVATE: instance %p", (void*)d);
            if (d && d->subview) {
                [d->subview stopAnimating];      // invalidate timer → break retain cycle
                [d->subview removeFromSuperview];
                [d->subview release];
                d->subview = nil;
            }
            if (d) { d->destView = nil; d->playing = false; }
            break;
        }

        case kVisualPluginFrameChangedMessage:
        case kVisualPluginWindowChangedMessage:
            // subview autoresizes; nothing to do.
            break;

        case kVisualPluginPulseMessage: {
            if (!d) break;
            const RenderVisualData* rd = messageInfo->u.pulseMessage.renderData;
            d->pulseCount++;
            d->renderTimeStampID = messageInfo->u.pulseMessage.timeStampID;
            d->lastPulseTime = CACurrentMediaTime();
            if (rd) {
                // Convert Music's UInt8 RenderVisualData into the shared float VizFrame.
                viz::VizFrame& f = d->frame;
                f.numSpectrumChannels = rd->numSpectrumChannels;
                f.numWaveformChannels = rd->numWaveformChannels;
                const int sch = rd->numSpectrumChannels < viz::kMaxChannels ? rd->numSpectrumChannels : viz::kMaxChannels;
                const int wch = rd->numWaveformChannels < viz::kMaxChannels ? rd->numWaveformChannels : viz::kMaxChannels;
                for (int ch = 0; ch < viz::kMaxChannels; ++ch) {
                    for (int i = 0; i < viz::kSpectrumBins; ++i)
                        f.spectrum[ch][i] = (ch < sch) ? rd->spectrumData[ch][i] / 255.0f : 0.f;
                    for (int i = 0; i < viz::kWaveformSamples; ++i)
                        f.waveform[ch][i] = (ch < wch) ? ((float)rd->waveformData[ch][i] - 128.f) / 128.f : 0.f;
                }
                f.computeBands();
                CFTimeInterval tNow = CACurrentMediaTime();
                double dt = d->haveData ? (tNow - f.time) : (1.0 / 60.0);
                f.time = tNow;
                // Music's spectrum is already log-ish 0..1, so ratios are
                // compressed: a more sensitive setting + higher floor.
                d->beat.sensitivity  = 1.5f;
                d->beat.silenceFloor = 0.02f;
                f.beat      = d->beat.process(f.bass, dt);
                f.bpm       = d->beat.bpm();
                f.beatPhase = d->beat.beatPhase();
                d->haveData = true;
                d->peakSpectrum = (UInt8)(f.peakSpectrum() * 255.0f + 0.5f);
            } else {
                d->frame.clear();
                d->peakSpectrum = 0;
            }
            // Log once per ~second to prove real data without flooding.
            if (d->pulseCount == 1 || (d->pulseCount % kPluginPulseRateHz) == 0) {
                os_log(gLog(),
                       "PULSE #%llu ts=%u specCh=%d waveCh=%d peakSpec=%d (rd=%p)",
                       (unsigned long long)d->pulseCount,
                       (unsigned)d->renderTimeStampID,
                       rd ? (int)rd->numSpectrumChannels : -1,
                       rd ? (int)rd->numWaveformChannels : -1,
                       (int)d->peakSpectrum, (const void*)rd);
            }
            break;
        }

        case kVisualPluginPlayMessage:
            if (d) d->playing = true;
            os_log(gLog(), "PLAY");
            break;

        case kVisualPluginStopMessage:
            if (d) { d->playing = false; d->frame.clear(); }
            os_log(gLog(), "STOP");
            break;

        case kVisualPluginChangeTrackMessage:
        case kVisualPluginSetPositionMessage:
        case kVisualPluginCoverArtMessage:
        case kVisualPluginConfigureMessage:
            break;

        default:
            status = unimpErr;
            break;
    }
    return status;
}

static OSStatus RegisterVisualPlugin(PluginMessageInfo* messageInfo)
{
    PlayerMessageInfo playerMessageInfo;
    memset(&playerMessageInfo.u.registerVisualPluginMessage, 0,
           sizeof(playerMessageInfo.u.registerVisualPluginMessage));

    PlayerRegisterVisualPluginMessage* reg = &playerMessageInfo.u.registerVisualPluginMessage;
    GetVisualName(reg->name);
    SetNumVersion(&reg->pluginVersion, 0, 1, developStage, 0);
    reg->options             = GetVisualOptions();
    reg->handler             = (VisualPluginProcPtr)VisualPluginHandler;
    reg->registerRefCon      = 0;
    reg->creator             = kPluginCreator;
    reg->pulseRateInHz       = kPluginPulseRateHz;
    reg->numWaveformChannels = 2;
    reg->numSpectrumChannels = 2;
    reg->minWidth            = 64;
    reg->minHeight           = 64;
    reg->maxWidth            = 0;
    reg->maxHeight           = 0;

    OSStatus status = PlayerRegisterVisualPlugin(messageInfo->u.initMessage.appCookie,
                                                 messageInfo->u.initMessage.appProc,
                                                 &playerMessageInfo);
    os_log(gLog(), "REGISTER: status=%d", (int)status);
    return status;
}

// ---------------------------------------------------------------------------
//  Bundle entry point — Music calls this by name after dlopen'ing the bundle.
// ---------------------------------------------------------------------------
extern "C" OSStatus iTunesPluginMainMachO(OSType message, PluginMessageInfo* messageInfo, void* refCon)
    __attribute__((visibility("default")));

extern "C" OSStatus iTunesPluginMainMachO(OSType message, PluginMessageInfo* messageInfo, void* refCon)
{
    (void)refCon;
    switch (message) {
        case kPluginInitMessage:    return RegisterVisualPlugin(messageInfo);
        case kPluginCleanupMessage: return noErr;
        default:                    return unimpErr;
    }
}
