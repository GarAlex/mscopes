//
// WVRenderView.mm — see WVRenderView.h.
//
#import "WVRenderView.h"
#import <Metal/Metal.h>
#import "FramebufferBlit.h"
#include "EngineLog.h"
#include "GpuFx.h"
#include <algorithm>
#include <cstdlib>

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

- (void)setExtendedRange:(BOOL)on
{
    CAMetalLayer* l = (CAMetalLayer*)self.layer;
    if (on) {
        // Display P3 primaries: the 8-bit layer has no colour space, so on
        // these (P3) screens its values already show as P3. The float layer
        // keeps the same colours; only the range above white is new.
        l.pixelFormat = MTLPixelFormatRGBA16Float;
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearDisplayP3);
        l.colorspace = cs;
        CGColorSpaceRelease(cs);
        l.wantsExtendedDynamicRangeContent = YES;
    } else {
        l.wantsExtendedDynamicRangeContent = NO;
        l.colorspace = nil;
        l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    }
    _edr = on;
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
        viz::PictureSettings s = pic ? *pic : viz::PictureSettings();
        s.pointScale = (float)self.layer.contentsScale;
        // HDR highlights on a screen with extended range (XDR): switch the
        // layer to linear float so highlights can shine above SDR white.
        // Everywhere else it stays the 8-bit layer, exactly as before.
        // MSCOPES_FORCE_EDR=1 uses the float layer on any screen (testing the
        // path on an SDR display, where headroom stays 1).
        static const bool forceEDR = getenv("MSCOPES_FORCE_EDR") != nullptr;
        NSScreen* screen = self.window.screen;
        const BOOL wantEDR = s.hdr && screen &&
            (forceEDR || screen.maximumPotentialExtendedDynamicRangeColorComponentValue > 1.01);
        if (wantEDR != _edr) {
            [self setExtendedRange:wantEDR];
            wvLog("display: %s (screen headroom now %.2f, potential %.2f)",
                  wantEDR ? "extended range on" : "extended range off",
                  screen ? screen.maximumExtendedDynamicRangeColorComponentValue : 1.0,
                  screen ? screen.maximumPotentialExtendedDynamicRangeColorComponentValue : 1.0);
        }
        s.linearOutput = _edr;
        s.headroom = _edr ? (float)std::max<CGFloat>(1.0, screen.maximumExtendedDynamicRangeColorComponentValue) : 1.f;
        viz::gpu::present(*fb, (__bridge void*)self.layer, s);
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
