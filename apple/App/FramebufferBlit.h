//
// FramebufferBlit.h — draw a viz::Framebuffer into the current NSGraphicsContext.
//
// CPU path: converts float RGBA → an 8-bit bitmap and draws it scaled to the
// destination rect (nearest/linear left to AppKit). The effect host renders at
// a fixed internal resolution; this scales it up to the view — like AVS did.
//
#pragma once
#import <Cocoa/Cocoa.h>
#include "Framebuffer.h"

namespace viz {

// Draws `fb` scaled into `dest`. Reuses `rep` across frames when the size
// matches (pass a strong reference the caller owns).
void BlitFramebuffer(const Framebuffer& fb, NSRect dest,
                     NSBitmapImageRep* __strong* rep);

} // namespace viz
