#!/usr/bin/env python3
"""
build-albums.py — detect numbered AVS preset "albums" (sequences meant to be
played in order — real AVS never clears between preset switches, and packs
like visbot's VE/"Visual Episode" series compose whole numbered shows around
that persistence, e.g. 01..10 track-numbered .avs files in one folder).

A folder qualifies as an album if it directly contains 2+ .avs files whose
name starts with a track number (optionally letter-prefixed: "01 ", "08 ",
"A08 ", "27_", "B13 ."). Tracks are sorted numerically.

Usage:
    python3 tools/build-albums.py [root-dir]      # default: presets/extracted
    python3 tools/build-albums.py --json out.json # also write a JSON catalog

Prints a human-readable summary; this is a review/validation tool — the app
does its own live scan of a user-chosen folder, this script exists to sanity
-check the detection heuristic and give a quick browsable overview.
"""
import json
import os
import re
import sys

TRACK_RE = re.compile(r"^([A-Za-z]?)(\d{1,3})[^0-9A-Za-z]")


def track_key(filename):
    m = TRACK_RE.match(filename)
    if not m:
        return None
    letter, num = m.groups()
    return (int(num), letter)


def find_albums(root):
    albums = []
    for dirpath, dirnames, filenames in os.walk(root):
        avs = [f for f in filenames if f.lower().endswith(".avs")]
        tracks = []
        for f in avs:
            k = track_key(f)
            if k is not None:
                tracks.append((k, f))
        if len(tracks) >= 2:
            tracks.sort(key=lambda t: t[0])
            albums.append({
                "path": dirpath,
                "name": os.path.basename(dirpath),
                "tracks": [t[1] for t in tracks],
            })
    return albums


def main():
    args = sys.argv[1:]
    json_out = None
    if "--json" in args:
        i = args.index("--json")
        json_out = args[i + 1]
        del args[i:i + 2]
    root = args[0] if args else os.path.join("presets", "extracted")

    if not os.path.isdir(root):
        sys.exit(f"not a directory: {root}")

    albums = find_albums(root)
    albums.sort(key=lambda a: -len(a["tracks"]))

    total_tracks = sum(len(a["tracks"]) for a in albums)
    print(f"== {len(albums)} albums found, {total_tracks} tracks total ==\n")
    for a in albums:
        print(f"{a['name']}  ({len(a['tracks'])} tracks)")
        print(f"  {a['path']}")
        for t in a["tracks"][:3]:
            print(f"    {t}")
        if len(a["tracks"]) > 3:
            print(f"    ... +{len(a['tracks']) - 3} more")
        print()

    if json_out:
        with open(json_out, "w") as f:
            json.dump(albums, f, indent=1)
        print(f"wrote {json_out}")


if __name__ == "__main__":
    main()
