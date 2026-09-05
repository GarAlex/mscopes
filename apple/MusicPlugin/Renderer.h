//
// Renderer.h — shared visualization renderer.
//
// Draws a VizFrame into the *current* NSGraphicsContext, filling `bounds`.
// Used by every front-end (Music plugin subview, standalone window view) so the
// look is identical regardless of where the audio comes from.
//
// ObjC++ (uses AppKit drawing primitives, available in every macOS front-end).
// This is the first shared component; the effect host will layer on top later.
//
#pragma once
#import <Cocoa/Cocoa.h>
#include "VizFrame.h"

namespace viz {

// Background + spectrum bars + waveform trace + an always-alive heartbeat dot.
// Call from a view's -drawRect: (which establishes the current context).
void DrawVisual(const VizFrame& frame, NSRect bounds);

} // namespace viz
