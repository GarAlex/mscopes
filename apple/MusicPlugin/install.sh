#!/usr/bin/env bash
#
# Install the built plugin into the Music/iTunes visualizer plug-ins folder.
#
# This is the same user-domain path the shipping projectM Apple Music plug-in
# installs into, which modern (sandboxed) Music.app still reads:
#     ~/Library/iTunes/iTunes Plug-ins/
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BUNDLE="${1:-$(ls -td "$REPO"/build/DerivedData/Build/Products/*/MScopesPlugin.bundle 2>/dev/null | head -1)}"

if [[ ! -d "$BUNDLE" ]]; then
  echo "!! no built bundle found. Build the MusicPlugin scheme first (see README)." >&2
  exit 1
fi

DEST="$HOME/Library/iTunes/iTunes Plug-ins"
echo ">> Installing to: $DEST"
mkdir -p "$DEST"
rm -rf "$DEST/MScopesPlugin.bundle"
cp -R "$BUNDLE" "$DEST/"

# Re-sign in place (ad-hoc) in case copying disturbed the signature.
# Keep a real (team / Developer ID) signature if the bundle has one; only
# ad-hoc sign an unsigned build.
if ! codesign --verify "$DEST/MScopesPlugin.bundle" > /dev/null 2>&1; then
  codesign --force --sign - "$DEST/MScopesPlugin.bundle"
fi
codesign -v "$DEST/MScopesPlugin.bundle" && echo "   signature OK"

# Report whether Music's sandbox container mirrors this path (informational).
CONTAINER="$HOME/Library/Containers/com.apple.Music/Data/Library/iTunes/iTunes Plug-ins"
if [[ -e "$CONTAINER" ]]; then
  echo ">> Note: Music container plug-ins path also exists:"
  echo "   $CONTAINER"
fi

cat <<'EOF'

>> Installed. To test on-device:
   1. Fully quit Music (Cmd-Q) if it is running, then reopen it.
   2. Start playing a track.
   3. Menu bar: View/Window > Visualizer  (turn it on).
   4. If multiple visualizers are listed, choose "MScopes".
      (In Music, the visualizer picker appears under the Visualizer menu
       once the visualizer is active.)

   Watch the on-screen HUD:
     - "pulses" climbing + moving bars  => Music DELIVERS audio data (success).
     - red "NO PULSE DATA FROM MUSIC"   => it loaded but gets no data on this OS.
     - visualizer not listed at all     => Music did not discover the bundle.

   Stream the plugin's log in a terminal:
     apple/MusicPlugin/logs.sh
EOF
