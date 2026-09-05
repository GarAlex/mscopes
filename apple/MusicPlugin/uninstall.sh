#!/usr/bin/env bash
# Remove the installed spike plugin.
set -euo pipefail
DEST="$HOME/Library/iTunes/iTunes Plug-ins/MScopesPlugin.bundle"
if [[ -d "$DEST" ]]; then
  rm -rf "$DEST"
  echo ">> Removed $DEST"
else
  echo ">> Nothing to remove at $DEST"
fi
echo ">> Quit and reopen Music for the change to take effect."
