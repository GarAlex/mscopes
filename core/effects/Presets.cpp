//
// Presets.cpp — effect registry + built-in preset library.
//
#include "Presets.h"
#include <set>
#include "BuiltinEffects.h"
#include "UtilEffects.h"
#include "LineMode.h"
#include "TransColor.h"
#include "TransFilter.h"
#include "TransGeo.h"
#include "RenderEffects.h"
#include "Superscope.h"
#include "DynamicMovement.h"
#include "ScriptedTrans.h"
#include "DelayEffects.h"
#include "GpuEffects.h"
#include "ModernEffects.h"

namespace viz {

template <typename T>
static EffectFactory make() { return [] { return std::unique_ptr<Effect>(new T()); }; }

const std::map<std::string, EffectFactory>& effectRegistry()
{
    static const std::map<std::string, EffectFactory> reg = {
        {"feedback_warp",  make<FeedbackWarp>()},
        {"blur",           make<BlurEffect>()},
        {"scope",          make<ScopeEffect>()},
        {"color_map",      make<ColorMapEffect>()},
        {"starfield",      make<StarfieldEffect>()},
        {"spectrum_bars",  make<SpectrumBarsEffect>()},
        {"clear_screen",   make<ClearScreenEffect>()},
        {"onbeat_clear",   make<OnBeatClearEffect>()},
        // TransColor (color transforms)
        {"brightness",      make<BrightnessEffect>()},
        {"invert",          make<InvertEffect>()},
        {"color_clip",      make<ColorClipEffect>()},
        {"colorfade",       make<ColorFadeEffect>()},
        {"color_reduction", make<ColorReductionEffect>()},
        {"unique_tone",     make<UniqueToneEffect>()},
        {"channel_shift",   make<ChannelShiftEffect>()},
        {"multiplier",      make<MultiplierEffect>()},
        {"fadeout",         make<FadeoutEffect>()},
        // TransFilter (convolution/water family)
        {"convolution",     make<ConvolutionEffect>()},
        {"multi_filter",    make<MultiFilterEffect>()},
        {"normalise",       make<NormaliseEffect>()},
        {"water",           make<WaterEffect>()},
        {"water_bump",      make<WaterBumpEffect>()},
        {"bump",            make<BumpEffect>()},
        // TransGeo (geometry transforms)
        {"mirror",          make<MirrorEffect>()},
        {"mosaic",          make<MosaicEffect>()},
        {"scatter",         make<ScatterEffect>()},
        {"grain",           make<GrainEffect>()},
        {"interleave",      make<InterleaveEffect>()},
        {"interferences",   make<InterferencesEffect>()},
        {"roto_blitter",    make<RotoBlitterEffect>()},
        {"add_borders",     make<AddBordersEffect>()},
        {"movement",        make<MovementEffect>()},
        // RenderEffects (scopes, dots, 3D)
        {"simple_scope",    make<SimpleScopeEffect>()},
        {"ring",            make<RingEffect>()},
        {"osc_star",        make<OscStarEffect>()},
        {"rot_star",        make<RotStarEffect>()},
        {"bass_spin",       make<BassSpinEffect>()},
        {"dot_grid",        make<DotGridEffect>()},
        {"dot_plane",       make<DotPlaneEffect>()},
        {"dot_fountain",    make<DotFountainEffect>()},
        {"timescope",       make<TimescopeEffect>()},
        {"moving_particle", make<MovingParticleEffect>()},
        // Scripted (EEL VM)
        {"superscope",      make<SuperscopeEffect>()},
        {"dynamic_movement",make<DynamicMovementEffect>()},
        {"color_modifier",  make<ColorModifierEffect>()},
        {"dynamic_shift",   make<DynamicShiftEffect>()},
        {"dynamic_distance",make<DynamicDistanceModifierEffect>()},
        {"global_variables",make<GlobalVariablesEffect>()},
        {"triangle",        make<TriangleEffect>()},
        {"texer2",          make<TexerIIEffect>()},
        {"buffer_save",     make<BufferSaveEffect>()},
        {"video_delay",     make<VideoDelayEffect>()},
        {"multi_delay",     make<MultiDelayEffect>()},
        {"custom_bpm",      make<CustomBPMEffect>()},
        {"set_render_mode", make<SetRenderModeEffect>()},
        // Modern (GPU, original to this project)
        {"bloom",           make<BloomEffect>()},
        {"kaleidoscope",    make<KaleidoscopeEffect>()},
        {"rgb_split",       make<RgbSplitEffect>()},
        {"pixel_shader",    make<PixelShaderEffect>()},
        {"tone_map",        make<ToneMapEffect>()},
        {"vignette",        make<VignetteEffect>()},
        {"trails",          make<TrailsEffect>()},
        {"particles",       make<ParticleSystemEffect>()},
        {"shockwave",       make<ShockwaveEffect>()},
        {"glitch",          make<GlitchEffect>()},
        {"streaks",         make<StreaksEffect>()},
        {"crt",             make<CRTEffect>()},
        {"shimmer",         make<ShimmerEffect>()},
        {"lens",            make<LensEffect>()},
        {"radial_blur",     make<RadialBlurEffect>()},
        {"neon_edges",      make<NeonEdgesEffect>()},
        {"duotone",         make<DuotoneEffect>()},
    };
    return reg;
}

void resetSharedState()
{
    eel::VM::resetShared();
    for (int i = 0; i < 8; ++i) {
        Framebuffer& b = globalBuffer(i);
        b.px.clear();
        b.w = b.h = 0;
    }
    resetDelayBuffers();
    lineMode() = LineMode{};          // AVS leaked this across presets; we don't (LineMode.h)
}

bool isModernEffectKey(const std::string& key)
{
    static const std::set<std::string> modern = {
        "bloom", "kaleidoscope", "rgb_split", "pixel_shader",
        "tone_map", "vignette", "trails", "particles", "spectrum_bars",
        "shockwave", "glitch", "streaks", "crt", "shimmer",
        "lens", "radial_blur", "neon_edges", "duotone",
    };
    return modern.count(key) > 0;
}

const std::vector<Preset>& builtinPresets()
{
    static const std::vector<Preset> presets = {
        // The default look: inward tunnel, glowing trails, crisp scope.
        { "Classic Tunnel", {
            {"feedback_warp", {{"zoom", 1.015f}, {"spin", 0.010f}, {"decay", 0.955f}}},
            {"blur",          {{"radius", 1}, {"mix", 1}}},
            {"scope",         {{"amp", 0.34f}, {"gain", 0.9f}}},
            {"color_map",     {{"cycle_on_beat", 1}}, /*enabled=*/false},
        }},
        // Aggressive inward rush remapped through the fire palette.
        { "Fire Storm", {
            {"feedback_warp", {{"zoom", 1.045f}, {"zoom_bass", 0.10f}, {"spin", 0.030f}, {"decay", 0.93f}}},
            {"blur",          {{"radius", 1}, {"mix", 1}}},
            {"scope",         {{"amp", 0.30f}, {"gain", 1.1f}}},
            {"color_map",     {{"palette", 0}, {"cycle_on_beat", 0}}},
        }},
        // Slow outward drift, heavy blur, ocean palette — ambient.
        { "Deep Ocean", {
            {"feedback_warp", {{"zoom", 0.985f}, {"zoom_bass", 0.02f}, {"spin", -0.008f},
                               {"spin_treble", 0.0f}, {"beat_kick", 0.005f}, {"decay", 0.98f}}},
            {"blur",          {{"radius", 2}, {"mix", 1}}},
            {"scope",         {{"amp", 0.24f}, {"gain", 1.0f}}},
            {"color_map",     {{"palette", 1}, {"cycle_on_beat", 0}}},
        }},
        // Warp-speed stars streaking through a fast-fading feedback field.
        { "Starflight", {
            {"feedback_warp", {{"zoom", 1.030f}, {"zoom_bass", 0.05f}, {"spin", 0.0f},
                               {"spin_treble", 0.0f}, {"beat_kick", 0.02f}, {"decay", 0.90f}}},
            {"starfield",     {{"speed", 0.4f}, {"speed_bass", 1.5f}, {"brightness", 1.0f}}},
            {"blur",          {{"radius", 1}, {"mix", 0.55f}}},
        }},
        // No warp motion at all: warp acts as pure fade; just the scope, clean.
        { "Bare Scope", {
            {"feedback_warp", {{"zoom", 1.0f}, {"zoom_bass", 0.0f}, {"spin", 0.0f},
                               {"spin_treble", 0.0f}, {"beat_kick", 0.0f}, {"decay", 0.80f}}},
            {"scope",         {{"amp", 0.40f}, {"gain", 1.0f}}},
        }},
        // Modern showcase: classic tunnel folded through the GPU effects —
        // things 8-bit AVS structurally couldn't do (HDR bloom, soft rolloff).
        { "Neon Cathedral", {
            {"feedback_warp", {{"zoom", 1.022f}, {"zoom_bass", 0.06f}, {"spin", 0.018f}, {"decay", 0.94f}}},
            {"scope",         {{"amp", 0.32f}, {"gain", 1.0f}}},
            {"kaleidoscope",  {{"segments", 6}, {"spin", 0.10f}, {"spin_beat", 0.5f}}},
            {"bloom",         {{"threshold", 0.5f}, {"radius", 18}, {"intensity", 1.3f}, {"bass_boost", 0.8f}}},
            {"rgb_split",     {{"amount", 1.5f}, {"beat_pump", 8}}},
        }, /*modern=*/true},
        // Particle fountain with phosphor trails, graded like film.
        { "Ember Field", {
            {"clear_screen",  {{"r", 0}, {"g", 0}, {"b", 0}}},
            {"trails",        {{"persistence", 0.82f}, {"mode", 1}, {"beat_flash", 0.3f}}},
            {"particles",     {{"emit_rate", 110}, {"beat_burst", 200}, {"speed", 0.65f},
                               {"gravity", -0.25f}, {"drag", 0.45f}, {"size", 3},
                               {"hue", 0.07f}, {"hue_spread", 0.12f}, {"lifetime", 2.2f}}},
            {"bloom",         {{"threshold", 0.5f}, {"radius", 12}, {"intensity", 0.9f}, {"bass_boost", 0.8f}}},
            {"vignette",      {{"inner", 0.5f}, {"outer", 1.3f}, {"strength", 0.7f}}},
            {"tone_map",      {{"exposure", 1.6f}, {"gamma", 1.05f}, {"saturation", 1.25f}}},
        }, /*modern=*/true},
        // Warp-speed rush: radial blur smears everything toward you; the
        // shockwave punches through it on the kick.
        { "Warp Core", {
            {"feedback_warp", {{"zoom", 1.035f}, {"zoom_bass", 0.08f}, {"spin", 0.012f}, {"decay", 0.87f}}},
            {"starfield",     {{"speed", 0.55f}, {"speed_bass", 2.0f}, {"brightness", 1.2f}}},
            {"scope",         {{"amp", 0.26f}, {"gain", 1.0f}}},
            {"radial_blur",   {{"amount", 0.10f}, {"beat_pump", 0.4f}}},
            {"shockwave",     {{"speed", 1.8f}, {"strength", 0.06f}}},
            {"vignette",      {{"inner", 0.5f}, {"outer", 1.2f}, {"strength", 0.7f}}},
        }, /*modern=*/true},
        // Molten glass: kaleidoscope through a bass-breathing lens + shimmer.
        { "Liquid Glass", {
            {"feedback_warp", {{"zoom", 1.018f}, {"spin", -0.015f}, {"decay", 0.90f}}},
            {"scope",         {{"amp", 0.30f}, {"gain", 1.1f}}},
            {"kaleidoscope",  {{"segments", 8}, {"spin", -0.08f}, {"spin_beat", 0.3f}}},
            {"lens",          {{"strength", 0.45f}, {"bass_boost", 0.6f}}},
            {"shimmer",       {{"amount", 3}, {"scale", 12}, {"bass_boost", 2}}},
            {"bloom",         {{"threshold", 0.62f}, {"radius", 14}, {"intensity", 0.85f}}},
            {"vignette",      {{"inner", 0.45f}, {"outer", 1.15f}, {"strength", 0.75f}}},
        }, /*modern=*/true},
        // Everything becomes a glowing wireframe; glitch rips it on beats.
        { "Neon Wire", {
            {"clear_screen",  {{"r", 0}, {"g", 0}, {"b", 0}}},
            {"scope",         {{"amp", 0.36f}, {"gain", 1.2f}}},
            {"ring",          {}},
            {"neon_edges",    {{"glow", 2.2f}, {"keep_source", 0.0f}}},
            {"trails",        {{"persistence", 0.72f}, {"mode", 1}}},
            {"glitch",        {{"intensity", 0.55f}, {"decay", 5}}},
            {"vignette",      {{"inner", 0.55f}, {"outer", 1.25f}, {"strength", 0.6f}}},
        }, /*modern=*/true},
        // Film noir in two colors: indigo shadows, coral highlights, grain
        // and streaks like an anamorphic print.
        { "Duotone Noir", {
            {"clear_screen",  {{"r", 0}, {"g", 0}, {"b", 0}}},
            {"trails",        {{"persistence", 0.8f}, {"mode", 0}}},
            {"particles",     {{"emit_rate", 90}, {"beat_burst", 150}, {"speed", 0.4f},
                               {"gravity", 0.05f}, {"size", 5}, {"hue", 0.08f}, {"lifetime", 3}}},
            {"scope",         {{"amp", 0.3f}, {"gain", 1.0f}}},
            {"streaks",       {{"threshold", 0.55f}, {"length", 50}, {"intensity", 1.1f}}},
            {"duotone",       {{"mix", 0.9f}}},
            {"grain",         {}},
            {"vignette",      {{"inner", 0.45f}, {"outer", 1.15f}, {"strength", 0.75f}}},
        }, /*modern=*/true},
        // The pixel shader's aurora over a slow tunnel, graded cold.
        { "Aurora", {
            {"feedback_warp", {{"zoom", 1.008f}, {"spin", 0.004f}, {"decay", 0.97f}}},
            {"pixel_shader",  {}},
            {"scope",         {{"amp", 0.22f}, {"gain", 0.8f}}},
            {"bloom",         {{"threshold", 0.55f}, {"radius", 20}, {"intensity", 1.0f}}},
            {"duotone",       {{"shadow_r", 0.02f}, {"shadow_g", 0.05f}, {"shadow_b", 0.15f},
                               {"high_r", 0.5f}, {"high_g", 1.0f}, {"high_b", 0.8f}, {"mix", 0.5f}}},
            {"vignette",      {{"inner", 0.6f}, {"outer", 1.3f}, {"strength", 0.55f}}},
        }, /*modern=*/true},
    };
    return presets;
}

void applyPreset(EffectHost& host, const Preset& preset)
{
    resetSharedState();
    host.clearEffects(!preset.inheritCanvas);
    host.inheritCanvas = preset.inheritCanvas;
    const auto& reg = effectRegistry();
    for (const auto& pe : preset.effects) {
        auto it = reg.find(pe.type);
        if (it == reg.end()) continue;
        auto fx = it->second();
        for (const auto& [key, value] : pe.params)
            fx->setParam(key, value);
        fx->enabled = pe.enabled;
        host.add(std::move(fx));
    }
}

} // namespace viz
