//
// Renderer.mm — shared visualization renderer (see Renderer.h).
//
#import "Renderer.h"
#include <cmath>

namespace viz {

void DrawVisual(const VizFrame& f, NSRect b)
{
    const CGFloat W = b.size.width, H = b.size.height;

    // background
    [[NSColor colorWithCalibratedRed:0.03 green:0.03 blue:0.05 alpha:1.0] setFill];
    NSRectFill(b);

    // --- spectrum bars (channel 0), aggregated kSpectrumBins -> kBars ---
    const int kBars = 64;
    const int perBar = kSpectrumBins / kBars;
    const CGFloat barW = W / (CGFloat)kBars;
    for (int bar = 0; bar < kBars; ++bar) {
        float sum = 0.f;
        for (int k = 0; k < perBar; ++k)
            sum += f.spectrum[0][bar * perBar + k];
        CGFloat avg = sum / (CGFloat)perBar;          // already 0..1
        if (avg < 0) avg = 0; if (avg > 1) avg = 1;
        CGFloat barH = avg * (H * 0.75);
        CGFloat x = bar * barW;
        NSColor* c = [NSColor colorWithCalibratedHue:(CGFloat)bar / kBars
                                          saturation:0.85
                                          brightness:0.55 + 0.45 * avg
                                               alpha:1.0];
        [c setFill];
        NSRectFill(NSMakeRect(x + 1, 0, barW - 2, barH + 1));
    }

    // --- waveform trace (channel 0) across vertical center ---
    NSBezierPath* wave = [NSBezierPath bezierPath];
    [wave setLineWidth:1.5];
    const CGFloat midY = H * 0.5;
    for (int i = 0; i < kWaveformSamples; ++i) {
        CGFloat x = (CGFloat)i / (kWaveformSamples - 1) * W;
        CGFloat s = f.waveform[0][i];                 // -1..1
        CGFloat y = midY + s * (H * 0.22);
        if (i == 0) [wave moveToPoint:NSMakePoint(x, y)];
        else        [wave lineToPoint:NSMakePoint(x, y)];
    }
    [[NSColor colorWithCalibratedWhite:1.0 alpha:0.85] setStroke];
    [wave stroke];

    // --- always-alive heartbeat dot (independent of audio) ---
    CGFloat phase = (CGFloat)(f.frameIndex % 120) / 120.0;
    CGFloat dotX = 12 + phase * (W - 24);
    [[NSColor colorWithCalibratedWhite:1.0 alpha:0.5] setFill];
    NSRectFill(NSMakeRect(dotX, H - 8, 4, 4));
}

} // namespace viz
