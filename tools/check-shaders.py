#!/usr/bin/env python3
"""Compile the Metal shaders embedded in the engine with Apple's offline
compiler, so a shader error fails the build instead of a user's Mac.

The GPU backend keeps its shader libraries in core/gpu/metal/shaders/*.msl.inc
as raw string literals, R"MSL( ... )MSL", which the backend includes and Metal
compiles when the app runs. Pixel Shader effects add their own kernels at run
time (EEL→MSL) and are checked by the self-tests instead. This compiles each
library with `xcrun -sdk macosx metal`. It needs Xcode's Metal compiler, not a
GPU, so it runs on CI too.

    tools/check-shaders.py            # exit 1 on any error
"""
import pathlib
import re
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
SHADERS = REPO / "core/gpu/metal/shaders"
BODY = re.compile(r'R"MSL\((.*?)\)MSL"', re.S)


def main() -> int:
    blocks = []
    for src in sorted(SHADERS.glob("*.msl.inc")):
        text = src.read_text()
        m = BODY.search(text)
        if m:
            # Compile the file itself minus the string delimiters, so line
            # numbers in errors are the file's.
            blocks.append((src.name, text[:m.start()] + m.group(1) + text[m.end():]))
    if not blocks:
        print(f"check-shaders: no shader libraries in {SHADERS.relative_to(REPO)} (have they moved?)")
        return 1
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, body in blocks:
            metal = pathlib.Path(tmp) / (name.split(".")[0] + ".metal")
            metal.write_text(body)
            r = subprocess.run(["xcrun", "-sdk", "macosx", "metal", "-c", str(metal),
                                "-o", str(metal.with_suffix(".air"))],
                               capture_output=True, text=True)
            lines = body.count("\n") + 1
            if r.returncode != 0:
                failed += 1
                print(f"FAIL {name} ({lines} lines)")
                print(r.stderr.replace(str(metal), name))
            else:
                warn = r.stderr.count("warning:")
                print(f"ok   {name} ({lines} lines{f', {warn} warnings' if warn else ''})")
    print(f"check-shaders: {len(blocks)} libraries, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
