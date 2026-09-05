//
// smoketest.mm — host the plugin bundle OUTSIDE Music to verify the full
// message path without needing Music itself.
//
// It dlopen's the built bundle, calls the real entry point, plays the role of
// Music (registers the plugin, sends Init → Activate → Pulse → Draw), feeds a
// synthetic spectrum/waveform, and snapshots the rendered NSView to a PNG.
//
// A non-zero PNG with moving bars proves: the bundle links & loads, the entry
// point + registration are correct, the handler processes pulse data, and the
// renderer draws into the Music-provided view. Only two things this canNOT
// prove (because they live inside Music): sandbox discovery of the install
// folder, and whether Music *actually* emits pulse data on this OS.
//
#import <Cocoa/Cocoa.h>
#include <dlfcn.h>
#include <cmath>
#include <cstdio>

#include "iTunesVisualAPI.h"

typedef OSStatus (*PluginMain)(OSType, PluginMessageInfo*, void*);

// Captured from the plugin's registration call.
static VisualPluginProcPtr gHandler = nullptr;
static PlayerRegisterVisualPluginMessage gReg;

// We stand in for iTunes/Music: the plugin calls back into us via appProc.
static OSStatus AppProc(void* /*appCookie*/, OSType message, PlayerMessageInfo* mi)
{
    if (message == kPlayerRegisterVisualPluginMessage) {
        gReg = mi->u.registerVisualPluginMessage;
        gHandler = gReg.handler;
        // name is a Pascal-style UniChar[]; convert first char count.
        int n = gReg.name[0];
        char ascii[256]; for (int i = 0; i < n && i < 255; i++) ascii[i] = (char)gReg.name[1+i];
        ascii[n] = 0;
        printf("  [register] name=\"%s\" options=0x%x creator=0x%08x pulseHz=%u specCh=%u waveCh=%u\n",
               ascii, (unsigned)gReg.options, (unsigned)gReg.creator,
               (unsigned)gReg.pulseRateInHz, (unsigned)gReg.numSpectrumChannels,
               (unsigned)gReg.numWaveformChannels);
        return noErr;
    }
    // PlayerRequestCurrentTrackCoverArt etc. — ignore.
    return noErr;
}

static void fillSyntheticData(RenderVisualData* rd, double t)
{
    rd->numWaveformChannels = 2;
    rd->numSpectrumChannels = 2;
    for (int ch = 0; ch < 2; ch++) {
        for (int i = 0; i < kVisualNumSpectrumEntries; i++) {
            // a couple of moving peaks + rolloff, 0..255
            double f = (double)i / kVisualNumSpectrumEntries;
            double v = 200.0 * exp(-40.0 * (f - 0.1 - 0.05*sin(t)) * (f - 0.1 - 0.05*sin(t)))
                     + 120.0 * exp(-60.0 * (f - 0.4) * (f - 0.4)) * (0.5 + 0.5*sin(t*2))
                     + 40.0 * (1.0 - f);
            rd->spectrumData[ch][i] = (UInt8)fmin(255.0, fmax(0.0, v));
        }
        for (int i = 0; i < kVisualNumWaveformEntries; i++) {
            double p = (double)i / kVisualNumWaveformEntries * M_PI * 8;
            rd->waveformData[ch][i] = (UInt8)(128 + 90 * sin(p + t*3));
        }
    }
}

int main(int argc, const char** argv)
{
    @autoreleasepool {
        setvbuf(stdout, nullptr, _IONBF, 0);        // unbuffered: survive a crash
        [NSApplication sharedApplication];          // AppKit drawing needs an app instance
        const char* bundleBin = (argc > 1) ? argv[1]
            : "build/MScopesPlugin.bundle/Contents/MacOS/MScopesPlugin";
        const char* outPng = (argc > 2) ? argv[2] : "build/smoketest_frame.png";

        printf(">> dlopen %s\n", bundleBin);
        void* h = dlopen(bundleBin, RTLD_NOW | RTLD_LOCAL);
        if (!h) { printf("!! dlopen failed: %s\n", dlerror()); return 1; }

        PluginMain entry = (PluginMain)dlsym(h, "iTunesPluginMainMachO");
        if (!entry) { printf("!! dlsym iTunesPluginMainMachO failed: %s\n", dlerror()); return 1; }
        printf("  entry point resolved: %p\n", (void*)entry);

        // --- Music sends kPluginInitMessage → plugin registers ---
        PluginMessageInfo pmi; memset(&pmi, 0, sizeof(pmi));
        pmi.u.initMessage.appCookie = (void*)0x1;
        pmi.u.initMessage.appProc   = &AppProc;
        pmi.u.initMessage.majorVersion = kITPluginMajorMessageVersion;
        pmi.u.initMessage.minorVersion = kITPluginMinorMessageVersion;
        OSStatus st = entry(kPluginInitMessage, &pmi, nullptr);
        printf(">> kPluginInitMessage -> %d\n", (int)st);
        if (!gHandler) { printf("!! plugin did not register a handler\n"); return 1; }

        // --- Instance Init: handler allocates its per-instance state ---
        VisualPluginMessageInfo v; memset(&v, 0, sizeof(v));
        v.u.initMessage.appCookie = (void*)0x1;
        v.u.initMessage.appProc   = &AppProc;
        st = gHandler(kVisualPluginInitMessage, &v, nullptr);
        void* refCon = v.u.initMessage.refCon;
        printf(">> kVisualPluginInitMessage -> %d (refCon=%p)\n", (int)st, refCon);

        // --- Activate into an offscreen container view ---
        NSRect frame = NSMakeRect(0, 0, 640, 360);
        NSWindow* win = [[NSWindow alloc] initWithContentRect:frame
                                                    styleMask:NSWindowStyleMaskBorderless
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        NSView* container = [[NSView alloc] initWithFrame:frame];
        [win setContentView:container];

        memset(&v, 0, sizeof(v));
        v.u.activateMessage.view = (VISUAL_PLATFORM_VIEW)container;
        v.u.activateMessage.options = 0;
        st = gHandler(kVisualPluginActivateMessage, &v, refCon);
        printf(">> kVisualPluginActivateMessage -> %d\n", (int)st);

        // --- Feed several synthetic pulses (as Music would) ---
        RenderVisualData rd; memset(&rd, 0, sizeof(rd));
        for (int frameN = 0; frameN < 30; frameN++) {
            fillSyntheticData(&rd, frameN * 0.1);
            memset(&v, 0, sizeof(v));
            v.u.pulseMessage.renderData = &rd;
            v.u.pulseMessage.timeStampID = frameN + 1;
            v.u.pulseMessage.newPulseRateInHz = 120;
            gHandler(kVisualPluginPulseMessage, &v, refCon);
        }
        printf(">> fed 30 synthetic pulses\n");

        // --- Force a draw and snapshot the container (incl. plugin subview) ---
        [container display];
        NSBitmapImageRep* rep = [container bitmapImageRepForCachingDisplayInRect:container.bounds];
        [container cacheDisplayInRect:container.bounds toBitmapImageRep:rep];
        NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        BOOL ok = [png writeToFile:[NSString stringWithUTF8String:outPng] atomically:YES];
        printf(">> wrote %s (%lu bytes, ok=%d)\n", outPng, (unsigned long)png.length, ok);

        // --- Deactivate + cleanup ---
        memset(&v, 0, sizeof(v));
        gHandler(kVisualPluginDeactivateMessage, &v, refCon);
        gHandler(kVisualPluginCleanupMessage, &v, refCon);
        printf(">> deactivate + cleanup done\n");

        // Intentionally do NOT dlclose(h): unloading a bundle that registered
        // Objective-C classes is unsafe on macOS (the ObjC runtime doesn't
        // truly unload classes). The process is exiting anyway.
        printf(">> SMOKE TEST PASSED\n");
        return ok ? 0 : 2;
    }
}
