# Music visual plugin

The engine running inside Apple Music's visualizer, as a classic iTunes-API
visual plugin (`MScopesPlugin.bundle`, universal arm64 + x86_64 because
Music's visualizer host can be the Intel helper process).

## Getting it — the easy way

Open **MScopes** (the app) and press **Install…** in the **Music Plugin**
box of the sidebar. The app ships the plugin inside its own bundle
(`Contents/PlugIns/`) and copies it to where Music looks:

    ~/Library/iTunes/iTunes Plug-ins/MScopesPlugin.bundle

Then **quit and reopen Music** (it scans that folder at launch), start a song,
and choose **View ▸ Visualizer ▸ MScopes**. Toggle the visualizer
with ⌘T. **Remove** in the same box deletes it again.

## From the source tree

    cd apple && xcodegen generate
    xcodebuild -project MScopes.xcodeproj -scheme MusicPlugin -configuration Release build
    apple/MusicPlugin/install.sh        # copies the newest built bundle, ad-hoc signs it

`uninstall.sh` removes it; `logs.sh` streams Music's log lines from the plugin
(`os_log` with the plugin's subsystem) while you test.

## What it does today

The plugin draws with the bar/waveform renderer (`Renderer.mm`) fed by
Music's own spectrum/waveform data, plus the shared `BeatDetector`. The effect
engine (`VizCore`) is linked but not routed into it yet, so it does not play
presets.

## Facts worth knowing

- Music hands the plugin pre-analysed audio (`RenderVisualData`: 512 spectrum
  bins + waveform, 0..255) at its own pulse rate; drawing happens on our own
  timer — the two are decoupled ("pulse ≠ draw").
- The entry point is `iTunesPluginMainMachO`, exported with default
  visibility while everything else is hidden.
- macOS 26 Music still delivers visualizer pulse data (verified on device).
- `smoketest.mm` is a tiny host harness that loads the bundle and drives the
  entry point without Music, for off-device checks.
