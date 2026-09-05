#!/usr/bin/env bash
#
# Fetch the community AVS preset corpora used for loader coverage testing.
# Downloads are NOT vendored into the repo (community art, licenses unclear);
# they land in build/packs/ (gitignored).
#
#   tools/fetch-presets.sh          # visbot archive (~570 packs, ~70MB)
#
# Corpora:
#  - visbot conformance corpus: github.com/visbot/avs-effects (1 effect/preset)
#  - the visbot archive: https://archive.visbot.net (community packs, .7z)
#
# Run the loader over everything afterwards:
#   AVS_FRAMES=32 AVS_NO_PNG=1 ./build/avs_selftest build/packs/extracted out/
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
OUT="$REPO/build/packs"
mkdir -p "$OUT/dl" "$OUT/extracted"

echo ">> conformance corpus (github.com/visbot/avs-effects)"
if [ ! -d "$REPO/build/avs-effects-corpus" ]; then
    git clone -q --depth 1 https://github.com/visbot/avs-effects.git \
        "$REPO/build/avs-effects-corpus"
fi

echo ">> crawling archive.visbot.net pack list"
python3 - "$OUT" <<'EOF'
import re, sys, urllib.request
out = sys.argv[1]
root = urllib.request.urlopen("https://visbot.net/archive/__data.json", timeout=30).read().decode("utf-8","replace")
folders = sorted(set(re.findall(r'"folders":\[([^\]]*)\]', root)))
# fallback: parse folder names from the archive index page data
names = sorted(set(re.findall(r'"((?:[a-zA-Z0-9_\-]|00b)+)"', folders[0]))) if folders else []
urls = []
for f in names:
    try:
        raw = urllib.request.urlopen(f"https://visbot.net/archive/{f}/__data.json", timeout=30).read().decode("utf-8","replace")
        urls += re.findall(r'https://files\.visbot\.net/[^"]+?\.(?:7z|zip)', raw)
    except Exception as e:
        print(f"{f}: {e}", file=sys.stderr)
open(f"{out}/pack_urls.txt","w").write("\n".join(sorted(set(urls)))+"\n")
print(f"{len(set(urls))} packs listed")
EOF

echo ">> downloading packs"
while read -r u; do
    f="$OUT/dl/$(echo "$u" | sed 's|https://files.visbot.net/archive/||; s|/|__|g')"
    [ -f "$f" ] || curl -sL --max-time 120 -o "$f" "$u" || echo "FAIL $u"
done < "$OUT/pack_urls.txt"

echo ">> extracting (bsdtar reads .7z)"
for f in "$OUT"/dl/*; do
    d="$OUT/extracted/$(basename "$f" | sed 's/\.[^.]*$//')"
    [ -d "$d" ] || { mkdir -p "$d"; bsdtar -xf "$f" -C "$d" 2>/dev/null || rmdir "$d"; }
done
echo ">> done: $(find "$OUT/extracted" -iname '*.avs' | wc -l | tr -d ' ') presets"
