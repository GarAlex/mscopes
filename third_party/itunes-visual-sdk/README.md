# iTunes Visual SDK (vendored)

These three files are Apple's **iTunes Visual Plug-in SDK** headers/support,
defining the visualizer contract that iTunes and modern **Music.app** still
implement:

- `iTunesAPI.h` — core types, plugin entry/registration messages, callbacks.
- `iTunesVisualAPI.h` — visual-plugin messages and `RenderVisualData`
  (512-entry spectrum + waveform, up to 2 channels).
- `iTunesAPI.cpp` — thin helpers (`PlayerRegisterVisualPlugin`, `SetNumVersion`, …).

## Provenance & license

Apple sample code, **© Apple Inc.**, distributed under Apple's sample-code
license (the full notice is retained at the top of each file): a personal,
non-exclusive license to use, reproduce, modify, and redistribute in source or
binary form, provided the notice is retained and Apple's name is not used to
endorse derived products.

This is **Apple's SDK**, not projectM's LGPL code. The copies here are byte-for-
byte the same headers the (LGPL) projectM Apple Music plug-in vendors, but they
carry Apple's license independently and are the canonical interface for any
iTunes/Music visualizer. Keeping them under `third_party/` keeps their license
cleanly separated from this repo's own (permissive) license.

Not legal advice — confirm before redistributing.
