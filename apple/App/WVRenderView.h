//
// WVRenderView.h — the engine's drawing view (private to the engine; the
// Swift side only sees it as VizEngine.renderView, an NSView).
//
//  Metal-backed: the view hosts a CAMetalLayer and gpu::present() draws the
//  engine's framebuffer straight into its drawable each tick. The old path —
//  float→8-bit conversion, NSBitmapImageRep drawInRect, and AppKit's
//  backing-store commit of a full-window view — capped the app at ~45 fps
//  even when the effects themselves took under 10 ms. Kept as the fallback
//  (drawRect) for machines without Metal.
//
#pragma once
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include "VizFrame.h"
#include "Picture.h"
#include "Framebuffer.h"

@interface WVRenderView : NSView
{
@public
    viz::VizFrame frame;             // latest audio frame (for the HUD readout)
    const viz::Framebuffer* fb;      // effect-host output to present (owned by engine)
    const viz::PictureSettings* pic; // display-stage looks (owned by engine; may be null)
    BOOL          showHUD;
    double        blitMs;            // last present/blit cost (profiling)
@private
    BOOL              _edr;          // the layer is an extended-range float layer right now
    NSBitmapImageRep* _blitRep;      // CPU fallback: reused across frames
    BOOL              _metal;
    CATextLayer*      _hud;
    int               _hudTick;
}
- (void)presentFrame;
@end
