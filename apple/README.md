# apple/ — the macOS products

One XcodeGen project ([`project.yml`](project.yml)) with three targets:

| target | product | notes |
|---|---|---|
| `VizCore` | static library | the shared engine (`../core`, minus tests/tools/non-Apple backends); universal |
| `App` | `MScopes.app` | SwiftUI app: system-audio capture, Metal presentation, preset browser; embeds the plugin |
| `MusicPlugin` | `MScopesPlugin.bundle` | Apple Music visual plugin (see [MusicPlugin/README.md](MusicPlugin/README.md)) |

    brew install xcodegen
    cd apple && xcodegen generate            # project.yml is the source of truth
    open MScopes.xcodeproj                 # or:
    xcodebuild -project MScopes.xcodeproj -scheme App -configuration Debug \
               -destination 'generic/platform=macOS' build

The `.xcodeproj` is generated and gitignored. Use the *generic* destination
from the command line: without it xcodebuild builds the active architecture
only, and the plugin must be universal (Music's visualizer host can be the
Intel helper). Building `App` also builds the
plugin and copies it into `MScopes.app/Contents/PlugIns/`, which is what the
app's **Install…** button installs for Music.

## Where the app finds things

- **Presets:** built-in stacks are compiled in. Everything else comes from the
  **library folder** you pick in the app (Albums box or the Preset Browser,
  ⌘B): every `.avs` underneath is listed, numbered sequences become albums.
  The folder is remembered (`UserDefaults` key `albumRootPath`).
- **The Music plugin:** inside the app bundle; installed to
  `~/Library/iTunes/iTunes Plug-ins/` on request.
- **Signing:** automatic with your team for local builds; Developer ID +
  notarization for distribution — see [`../docs/SIGNING.md`](../docs/SIGNING.md).
  The App Store is not an option: its sandbox blocks the system-wide audio tap.

## Dev aids

- `MSCOPES_PRESET=<file.avs|.json>` loads a preset at launch (profiling from a shell).
- `WV_POLL_HZ=<n>` changes the sidebar readout rate.
- `kill -USR1 <pid>` writes the window and the raw engine frame as PNGs to the temp
  directory (path is logged).
- `kill -USR2 <pid>` stops the audio tap's IO in place (what a device change or
  sleep does to it); the engine's watchdog must re-open it within ~3 s and the
  sidebar shows "reconnected ×1".
- `MSCOPES_NO_GPU=1` forces every effect's CPU path.
