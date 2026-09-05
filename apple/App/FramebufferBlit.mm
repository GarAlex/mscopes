//
// FramebufferBlit.mm
//
#import "FramebufferBlit.h"
#include <algorithm>

namespace viz {

void BlitFramebuffer(const Framebuffer& fb, NSRect dest,
                     NSBitmapImageRep* __strong* rep)
{
    if (fb.w == 0 || fb.h == 0) return;

    NSBitmapImageRep* r = *rep;
    if (!r || r.pixelsWide != fb.w || r.pixelsHigh != fb.h) {
        r = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
            pixelsWide:fb.w pixelsHigh:fb.h bitsPerSample:8 samplesPerPixel:4
            hasAlpha:YES isPlanar:NO
            colorSpaceName:NSDeviceRGBColorSpace
            bytesPerRow:fb.w * 4 bitsPerPixel:32];
        *rep = r;
    }

    unsigned char* dst = r.bitmapData;
    const float* src = fb.px.data();
    const int n = fb.w * fb.h;
    for (int i = 0; i < n; ++i) {
        // clamp float 0..1 → 0..255
        float rr = src[i*4+0], gg = src[i*4+1], bb = src[i*4+2];
        dst[i*4+0] = (unsigned char)(std::min(1.f, std::max(0.f, rr)) * 255.f);
        dst[i*4+1] = (unsigned char)(std::min(1.f, std::max(0.f, gg)) * 255.f);
        dst[i*4+2] = (unsigned char)(std::min(1.f, std::max(0.f, bb)) * 255.f);
        dst[i*4+3] = 255;
    }

    // Flip vertically while drawing: Framebuffer row 0 is bottom (math coords),
    // NSBitmapImageRep row 0 is top. Use a transform so the image isn't upside down.
    NSGraphicsContext* ctx = [NSGraphicsContext currentContext];
    [ctx saveGraphicsState];
    ctx.imageInterpolation = NSImageInterpolationHigh;  // crisp upscale (buffer is ~half view size)
    NSAffineTransform* t = [NSAffineTransform transform];
    [t translateXBy:0 yBy:NSMaxY(dest) + NSMinY(dest)];
    [t scaleXBy:1 yBy:-1];
    [t concat];
    [r drawInRect:dest];
    [ctx restoreGraphicsState];
}

} // namespace viz
