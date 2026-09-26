# Presets: where to get them, where the app looks

The app ships with its own built-in presets only. Everything from the
Winamp/AVS era — the thousands of community `.avs` presets — is content you
bring yourself; nothing of it is bundled, tracked, or redistributed here.

## Where the app looks

Pick a **library folder** once — the Albums box in the sidebar, or the Preset
Browser (⌘B) → Library → *Choose Library Folder…*. The app scans it
recursively:

- every `.avs` file underneath is listed in the browser's **Library** source,
  with its top-level folder ("pack") as subtitle, searchable;
- folders holding 2+ files that start with a track number (`01 …`, `08 …`,
  `A08 …`) become **Albums** — sequences meant to play in order, where later
  tracks warp what the previous one left on screen. Play them from the
  browser's Albums source; the sidebar drives track order and auto-advance.

The folder path is remembered (`UserDefaults` → `albumRootPath`); re-scans
happen at launch and whenever you change it. 13,000 files index in well
under a second.

Our own native format is JSON (`Save…` / `Open…`); `.avs` files load through
the classic-format reader and can be saved back out as JSON.

## Where to get presets

- **Your own Winamp install:** `C:\Program Files\Winamp\Plugins\avs\` holds
  everything you ever installed there; copy that folder over.
- **Preset packs from the community archives** — historically distributed as
  Winamp installers (NSIS `.exe`) or zips. The visbot archive
  (`archive.visbot.net`) mirrors hundreds of packs; `tools/fetch-presets.sh`
  downloads it and `tools/sort-presets.py` unpacks the installers recursively
  (needs `unar`: `brew install unar`) into `presets/extracted/`, pack by
  pack, with a `PACK_INFO.txt` per pack. Point the app at that folder.
- **WinampHeritage** and similar sites list packs by author with previews.

These files are other people's art. Use them locally; don't repackage them
with the app.

## What loads, what doesn't

The reader understands the AVS 0.1/0.2 binary format completely — nested
Effect Lists with every blend mode, all scripted effects (Superscope,
Dynamic Movement/Shift/Distance, Color Modifier, Triangle, Texer II, Global
Variables), the built-in effects, Set Render Mode, Buffer Save, Custom BPM,
and the common APEs. Deliberately unsupported: Text, Picture, SVP/Video
(Windows codecs), and APEs that were external Windows DLLs (FyrewurX, Flock
Off…). A preset whose only components are those loads empty; everything
else renders. Over the full community archive that is 10 files in 13,440.

## Extra script variables

Scripts in Superscope, Dynamic Movement, the scripted transforms and the
pixel shader can read these, besides AVS's own and `bass`, `mid`, `treb`,
`bpm`, `beatphase`:

| Variable | Value |
|---|---|
| `kick`, `snare`, `hat` | 1 at a hit of that drum, decaying toward 0 |
| `onset` | how percussive the sound is right now, 0..1 |
| `level` | overall loudness, 0..1, with automatic gain |
| `bass_att`, `mid_att`, `treb_att` | slow averages of lows, mids and highs, 0..1 |

A preset that assigns one of these names in its own scripts keeps it as its
own variable, so older presets that happen to use the same names behave as
they always did.

## "It's just black"

Some presets are **modifiers**: they contain no scope or renderer, only
transforms authored to warp whatever the previous preset left on screen —
common as later tracks of an album. Opened alone they show nothing; play the
album, or load a preset with content first (the app inherits the canvas for
modifier presets automatically).
