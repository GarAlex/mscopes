//
// VizEngine.h — Objective-C bridge exposing the C++ visualization core to Swift.
//
// This header is intentionally PURE Objective-C (no C++), so it is safe to
// import from the Swift bridging header. The C++ pieces (audio tap, FFT
// analyzer, renderer) live behind it in VizEngine.mm.
//
#import <AppKit/AppKit.h>

NS_ASSUME_NONNULL_BEGIN

/// Owns the audio capture → analysis → render pipeline and vends the NSView
/// that draws the visualization. SwiftUI drives it via an ObservableObject.
@interface VizEngine : NSObject

/// The visualization surface. Embed via NSViewRepresentable.
@property (nonatomic, readonly) NSView *renderView;

/// Live status (poll from the UI).
@property (nonatomic, readonly, getter=isCapturing) BOOL capturing;
@property (nonatomic, readonly) double sampleRate;
@property (nonatomic, readonly) NSInteger channels;
@property (nonatomic, readonly) float peak;     // 0..1
@property (nonatomic, readonly) float bass;      // 0..1
@property (nonatomic, readonly) float mid;       // 0..1
@property (nonatomic, readonly) float treble;    // 0..1
@property (nonatomic, readonly) float bpm;       // tempo estimate, 0 until locked
@property (nonatomic, readonly) float beatLevel; // 1 on a detected beat, decays to 0
@property (nonatomic, readonly) unsigned long long audioCallbacks;

/// User-adjustable (demonstrates the SwiftUI → core control path).
@property (nonatomic) float sensitivity;         // 0.25 .. 4.0, default 1.0
@property (nonatomic) BOOL showHUD;              // in-view diagnostic overlay

/// Presets — selecting one rebuilds the effect stack.
@property (nonatomic, readonly) NSArray<NSString *> *presetNames;
@property (nonatomic) NSInteger currentPreset;

/// Effect stack (order = render order; changes when a preset is applied).
@property (nonatomic, readonly) NSArray<NSString *> *effectNames;
- (BOOL)isEffectEnabledAt:(NSInteger)index;
- (void)setEffect:(NSInteger)index enabled:(BOOL)enabled;

/// Effect browser: the full registry, and stack editing.
/// availableEffects returns registry entries as "key|Display Name", sorted by name.
@property (nonatomic, readonly) NSArray<NSString *> *availableEffects;
- (void)addEffectWithKey:(NSString *)key;
- (void)removeEffectAt:(NSInteger)index;
- (void)moveEffectAt:(NSInteger)from to:(NSInteger)to;

/// Load a classic Winamp .avs preset file, replacing the stack.
/// Returns a human-readable load report (or error description).
- (NSString *)loadAvsPresetAtPath:(NSString *)path;

/// Load one track of a numbered preset "album" (sequence). isFirst=YES
/// behaves like loadAvsPresetAtPath: (crossfade in, normal clear/inherit
/// choice); isFirst=NO loads with no crossfade and forces canvas
/// inheritance, matching how these sequences were authored to flow.
- (NSString *)loadAlbumTrackAtPath:(NSString *)path isFirst:(BOOL)isFirst;

/// Script-coordinate aspect mode: 0 stretch (classic AVS), 1 fill (round
/// shapes, crops the short axis — default), 2 fit.
- (void)setAspectMode:(NSInteger)mode;
- (NSInteger)aspectMode;

/// Shared script registers (reg00-99) — the app's macro knobs write these.
- (void)setGlobalReg:(NSInteger)index value:(float)value;
- (float)globalReg:(NSInteger)index;

/// Save / load the stack in our native JSON preset format.
/// Both return a human-readable status string.
- (NSString *)saveJsonPresetToPath:(NSString *)path name:(NSString *)name;
- (NSString *)loadJsonPresetAtPath:(NSString *)path;

/// Parameters of the effect at `index`: entries are "key|min|max|value|reg"
/// (reg = bound global register 0-99, or -1 when unbound).
- (NSArray<NSString *> *)paramsForEffectAt:(NSInteger)index;

/// Drive a param from shared register regNN each frame (reg -1 unbinds).
/// Persisted in JSON presets.
- (void)bindParamForEffectAt:(NSInteger)index key:(NSString *)key toReg:(NSInteger)reg;

/// Scripted effects (Superscope / Dynamic Movement): nil if not scriptable,
/// else @[init, frame, beat, point].
- (NSArray<NSString *> *)scriptsForEffectAt:(NSInteger)index;
- (void)setScriptsForEffectAt:(NSInteger)index
                         initScript:(NSString *)i frameScript:(NSString *)f
                         beatScript:(NSString *)b pointScript:(NSString *)p;
- (void)setParamForEffectAt:(NSInteger)index key:(NSString *)key value:(float)value;

/// Start/stop system-audio capture. Returns NO and sets *error on failure.
- (BOOL)start:(NSError *_Nullable *_Nullable)error;
- (void)stop;

@end

NS_ASSUME_NONNULL_END
