#!/usr/bin/env python3
"""
sort-presets.py — re-extract every downloaded visbot archive pack (some are
NSIS/Inno self-extracting installers that `bsdtar` can't open, only `unar`
can) and sort the result into two trees parallel to presets/classic/:

  presets/extracted/<pack-id>/   packs that yielded at least one .avs file
                                  (the .avs files + textures, flattened out
                                  of installer noise like Fonts/, Registry
                                  hive dumps, uninstaller stubs), plus a
                                  PACK_INFO.txt describing where it came from.

  presets/failed/<pack-id>/      packs that still didn't yield any .avs —
                                  the ORIGINAL untouched downloaded file, for
                                  manual review (e.g. run the .exe on
                                  Windows/Wine, or it's a genuinely non-AVS
                                  download), plus a PACK_INFO.txt with the
                                  failure reason.

Both trees are for local review only — NOT committed (see .gitignore) since
this is community art with unclear redistribution rights, same policy as
build/packs/.

Requires `unar` (brew install unar) — handles zip/7z/rar/NSIS/InnoSetup.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DL_DIR = os.path.join(REPO, "build", "packs", "dl")
URLS_FILE = os.path.join(REPO, "build", "packs", "pack_urls.txt")
OUT_EXTRACTED = os.path.join(REPO, "presets", "extracted")
OUT_FAILED = os.path.join(REPO, "presets", "failed")

# Installer cruft we don't want copied into presets/extracted/.
SKIP_DIR_NAMES = {"windows directory", "fonts", "start menu", "$plugins",
                  "$recycle.bin", "program files"}
SKIP_EXTS = {".exe", ".dll", ".ttf", ".fon", ".lnk", ".url", ".ini", ".chm",
            ".hlp", ".manifest", ".log"}


def load_url_map():
    """pack-id (as used in dl/ filenames) -> source URL."""
    m = {}
    if not os.path.exists(URLS_FILE):
        return m
    for line in open(URLS_FILE):
        url = line.strip()
        if not url:
            continue
        # https://files.visbot.net/archive/<artist>/<pack>.<ext> ->
        # <artist>__<pack> (matches how the downloader named dl/ files)
        rest = url.split("/archive/", 1)[-1]
        artist, _, fname = rest.partition("/")
        base = re.sub(r"\.[^.]+$", "", fname)
        pack_id = f"{artist}__{base}"
        m[pack_id] = url
    return m


def find_avs_files(root):
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d.lower() not in SKIP_DIR_NAMES]
        for fn in filenames:
            if fn.lower().endswith(".avs"):
                out.append(os.path.join(dirpath, fn))
    return out


NESTED_EXTS = (".exe", ".msi", ".zip", ".7z", ".rar")


def extract_recursive(src_path, dest_dir, depth=0, max_depth=3):
    """unar src_path into dest_dir; if no .avs turn up, unar any nested
    installer/archive files found (one level at a time) in place. Returns
    True if unar itself opened src_path successfully."""
    proc = subprocess.run(
        ["unar", "-quiet", "-force-overwrite", "-output-directory", dest_dir, src_path],
        capture_output=True, text=True, timeout=180)
    if proc.returncode != 0:
        return False
    if depth >= max_depth:
        return True
    if find_avs_files(dest_dir):
        return True
    for dirpath, _, filenames in os.walk(dest_dir):
        for fn in filenames:
            if fn.lower().endswith(NESTED_EXTS):
                nested = os.path.join(dirpath, fn)
                extract_recursive(nested, dirpath, depth + 1, max_depth)
    return True


def copy_pack_content(root, dest, avs_files):
    """Copy .avs files + sibling non-installer-cruft resources (textures,
    readmes), preserving relative layout, skipping installer artifacts."""
    keep_dirs = set()
    for f in avs_files:
        keep_dirs.add(os.path.dirname(f))
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d.lower() not in SKIP_DIR_NAMES]
        rel_dir = os.path.relpath(dirpath, root)
        for fn in filenames:
            ext = os.path.splitext(fn)[1].lower()
            if ext in SKIP_EXTS:
                continue
            src = os.path.join(dirpath, fn)
            dst = os.path.join(dest, rel_dir, fn) if rel_dir != "." else os.path.join(dest, fn)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)


def main():
    if shutil.which("unar") is None:
        sys.exit("unar not found — brew install unar")

    os.makedirs(OUT_EXTRACTED, exist_ok=True)
    os.makedirs(OUT_FAILED, exist_ok=True)
    url_map = load_url_map()

    files = sorted(f for f in os.listdir(DL_DIR)
                   if os.path.isfile(os.path.join(DL_DIR, f)))
    n_ok = n_fail = 0
    fail_reasons = {}

    for i, fname in enumerate(files, 1):
        pack_id = re.sub(r"\.[^.]+$", "", fname)
        src_path = os.path.join(DL_DIR, fname)
        url = url_map.get(pack_id, "(unknown — not in pack_urls.txt)")

        with tempfile.TemporaryDirectory() as tmp:
            opened_ok = extract_recursive(src_path, tmp)
            avs = find_avs_files(tmp) if opened_ok else []

            if avs:
                dest = os.path.join(OUT_EXTRACTED, pack_id)
                if os.path.exists(dest):
                    shutil.rmtree(dest)
                os.makedirs(dest, exist_ok=True)
                copy_pack_content(tmp, dest, avs)
                with open(os.path.join(dest, "PACK_INFO.txt"), "w") as f:
                    f.write(f"pack_id: {pack_id}\n")
                    f.write(f"source_url: {url}\n")
                    f.write(f"source_file: {fname}\n")
                    f.write(f"avs_files: {len(avs)}\n")
                    f.write("extracted_with: unar\n")
                n_ok += 1
            else:
                dest = os.path.join(OUT_FAILED, pack_id)
                if os.path.exists(dest):
                    shutil.rmtree(dest)
                os.makedirs(dest, exist_ok=True)
                shutil.copy2(src_path, os.path.join(dest, fname))
                reason = "no .avs files found after extraction (incl. nested archives)" \
                    if opened_ok else "unar could not open the file"
                with open(os.path.join(dest, "PACK_INFO.txt"), "w") as f:
                    f.write(f"pack_id: {pack_id}\n")
                    f.write(f"source_url: {url}\n")
                    f.write(f"source_file: {fname}\n")
                    f.write(f"status: FAILED\n")
                    f.write(f"reason: {reason}\n")
                n_fail += 1
                fail_reasons[pack_id] = reason

        if i % 50 == 0 or i == len(files):
            print(f"[{i}/{len(files)}] ok={n_ok} fail={n_fail}", file=sys.stderr)

    print(f"\n== done: {n_ok} extracted, {n_fail} failed ==")
    total_avs = sum(len(find_avs_files(os.path.join(OUT_EXTRACTED, d)))
                    for d in os.listdir(OUT_EXTRACTED))
    print(f"total .avs files recovered: {total_avs}")


if __name__ == "__main__":
    main()
