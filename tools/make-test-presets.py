#!/usr/bin/env python3
"""
make-test-presets.py — write the repo's own .avs test fixtures.

These are the presets the test suite loads and renders. They are authored
here, not taken from any archive, and are designed to exercise the loader:
root and nested list headers (plain and 0x80 extended), every Effect List
blend mode, both script-block formats (versioned strings and the ancient
4x256 fixed buffers), APE-id components, Global Variables' C-string
layout, and each effect with a dedicated decoder. Scripts are ours.

    python3 tools/make-test-presets.py            # writes presets/tests/*.avs

Format reference: core/preset/AvsPreset.cpp (which mirrors vis_avs).
"""
import os
import struct

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "presets", "tests")
MAGIC02 = b"Nullsoft AVS Preset 0.2\x1a"
MAGIC01 = b"Nullsoft AVS Preset 0.1\x1a"


def i32(v):
    return struct.pack("<i", v)


def u32(v):
    return struct.pack("<I", v)


def avs_str(s):
    b = s.encode("latin-1") + b"\0"
    return u32(len(b)) + b


def scripts(point="", frame="", beat="", init=""):
    """Versioned script block: 1 + point, frame, beat, init."""
    return b"\x01" + avs_str(point) + avs_str(frame) + avs_str(beat) + avs_str(init)


def scripts_ancient(point="", frame="", beat="", init=""):
    """The pre-2.0 layout: four NUL-terminated 256-byte buffers."""
    def fixed(s):
        return s.encode("latin-1")[:255].ljust(256, b"\0")
    return fixed(point) + fixed(frame) + fixed(beat) + fixed(init)


def rgb(r, g, b):
    """AVS color int: 0x00BBGGRR."""
    return (b << 16) | (g << 8) | r


def comp(cid, body=b"", ape=None):
    out = i32(cid)
    if ape is not None:
        out += ape.encode("latin-1").ljust(32, b"\0")[:32]
    return out + i32(len(body)) + body


APE_ID = 16384   # components with a 32-byte name follow this id


def list_body(children, clear=False, enabled=True, in_blend=None, out_blend=None,
              in_adj=128, out_adj=128, in_buf=0, out_buf=0, on_beat_fields=False):
    """Effect List body: mode byte(s) + optional extended config + children.

    The extended block's length byte is what the loader adds 5 to; it reads
    six ints then (guarded by pos+4 < ext) up to two on-beat ints, and the
    children start where it stops — so the byte must be sized to land there.
    """
    first = (0 if enabled else 2) | (1 if clear else 0)
    kids = b"".join(children)
    if in_blend is None and out_blend is None:
        return bytes([first]) + kids
    first |= 0x80
    fields = [in_adj, out_adj, in_buf, out_buf, 0, 0]
    ext = b"".join(i32(v) for v in fields)
    length_byte = len(ext)
    if on_beat_fields:
        ext += i32(0) + i32(0)
        length_byte = len(ext) + 4
    ib = in_blend if in_blend is not None else 1
    ob = out_blend if out_blend is not None else 1
    hdr = bytes([first, 0, ib & 0x3F, (ob ^ 1) & 0x3F, length_byte])
    return hdr + ext + kids


def preset(root, magic=MAGIC02):
    return magic + root


# --- component builders (bodies per the loader's decoders) -------------------
def superscope(init, frame, beat, point, channel=2, spectrum=False, ancient=False):
    blk = scripts_ancient(point, frame, beat, init) if ancient else scripts(point, frame, beat, init)
    return comp(36, blk + i32((4 if spectrum else 0) | channel))


def dynamic_movement(init, frame, beat, point, grid_w=16, grid_h=12, rect=False, blend=False, wrap=True):
    return comp(43, scripts(point, frame, beat, init)
                + i32(1) + i32(1 if rect else 0) + i32(grid_w - 1) + i32(grid_h - 1)
                + i32(1 if blend else 0) + i32(1 if wrap else 0))


def dynamic_shift(init, frame, beat, blend=False, bilinear=True):
    return comp(42, b"\x01" + avs_str(init) + avs_str(frame) + avs_str(beat)
                + i32(1 if blend else 0) + i32(1 if bilinear else 0))


def dynamic_distance(init, frame, beat, point, blend=False, bilinear=True):
    return comp(35, scripts(point, frame, beat, init) + i32(1 if blend else 0) + i32(1 if bilinear else 0))


def color_modifier(init, frame, beat, point, recompute=True):
    return comp(45, scripts(point, frame, beat, init) + i32(1 if recompute else 0))


def blur(level=1):            # 1 medium, 2 light, 3 heavy
    return comp(6, i32(level))


def fadeout(speed=16, color=(0, 0, 0)):
    return comp(3, i32(speed) + i32(rgb(*color)))


def clear_screen(color=(0, 0, 0), only_first=False, blend_5050=False):
    return comp(25, i32(1) + i32(rgb(*color)) + i32(0) + i32(1 if blend_5050 else 0) + i32(1 if only_first else 0))


def set_render_mode(blend=1, alpha=255, width=1, enabled=True):
    return comp(40, u32((0x80000000 if enabled else 0) | (blend & 0xFF) | ((alpha & 0xFF) << 8) | ((width & 0xFF) << 16)))


def buffer_save(action=0, buf=0, blend=0, adjustable=128):
    return comp(18, i32(action) + i32(buf) + i32(blend) + i32(adjustable))


def custom_bpm(ms=500, skip=1, skip_first=0):
    return comp(33, i32(1) + i32(1) + i32(0) + i32(0) + i32(ms) + i32(skip) + i32(skip_first))


def on_beat_clear(color=(0, 0, 0), blend=False, n_beats=1):
    return comp(5, i32(rgb(*color)) + i32(1 if blend else 0) + i32(n_beats))


def invert(enabled=True):
    return comp(37, i32(1 if enabled else 0))


def water():
    return comp(20, i32(1))


def moving_particle(color=(255, 255, 255), distance=16, size=8, on_beat_size=24, blend=1):
    return comp(8, i32(1 | 2) + i32(rgb(*color)) + i32(distance) + i32(size) + i32(on_beat_size) + i32(blend))


def generic(cid, *ints):
    """Effects the loader instantiates from the registry with defaults."""
    return comp(cid, b"".join(i32(v) for v in ints))


def ape(name, *ints):
    return comp(APE_ID, b"".join(i32(v) for v in ints), ape=name)


def global_variables(init, frame, beat):
    def cstr(s):
        return s.encode("latin-1") + b"\0"
    return comp(APE_ID, i32(0) + b"\0" * 24 + cstr(init) + cstr(frame) + cstr(beat) + cstr(""),
                ape="Jheriko: Global")


def nested(children, **kw):
    return comp(-2, list_body(children, **kw))


# --- our scripts --------------------------------------------------------------
SCOPE_INIT = "n=240;t=0;"
SCOPE_FRAME = "t=t+0.03;"
SCOPE_BEAT = "t=t+0.5;"
SCOPE_POINT = ("x=i*2-1; y=v*0.4+sin(i*6.2832+t)*0.35;"
               "red=0.5+0.5*sin(t+i*3); green=1-i; blue=i;")

WHEEL_POINT = ("r=i*6.2832+t; d=0.55+v*0.25;"
               "x=cos(r)*d; y=sin(r)*d*0.75; red=1; green=0.4+0.6*i; blue=0.2;")

DM_POINT = "d=d*0.985; r=r+0.02*sin(d*3);"
DDM_POINT = "d=d*dd;"
DSHIFT_FRAME = "t=t+0.1; x=sin(t)*4; y=cos(t*0.7)*3;"
CM_POINT = "red=pow(red,0.8); green=green*0.9; blue=1-blue*0.5;"

# --- the fixtures ------------------------------------------------------------
FIXTURES = {}

FIXTURES["01-superscope-basic"] = preset(list_body([
    superscope(SCOPE_INIT, SCOPE_FRAME, SCOPE_BEAT, SCOPE_POINT),
], clear=True))

FIXTURES["02-feedback-movement"] = preset(list_body([
    clear_screen(only_first=True),
    superscope(SCOPE_INIT, SCOPE_FRAME, SCOPE_BEAT, WHEEL_POINT),
    generic(15, 2),                       # Movement: registry defaults (Big Swirl Out)
    blur(1),
    fadeout(8),
]))

# Every Effect List output blend mode, each list wrapping a scope; the last
# one carries the two on-beat ints so both extended-header loops run.
blend_lists = []
for mode in range(0, 14):
    blend_lists.append(nested(
        [superscope(SCOPE_INIT, SCOPE_FRAME, "", SCOPE_POINT.replace("i*3", "i*%d" % (mode + 2)))],
        in_blend=1, out_blend=mode, in_adj=200, out_adj=96,
        in_buf=0, out_buf=(2 if mode == 12 else 0),
        on_beat_fields=(mode == 13)))
FIXTURES["03-nested-list-blend-modes"] = preset(list_body([fadeout(12)] + blend_lists))

FIXTURES["04-dynamic-movement"] = preset(list_body([
    superscope(SCOPE_INIT, SCOPE_FRAME, SCOPE_BEAT, WHEEL_POINT),
    dynamic_movement("", "", "", DM_POINT, grid_w=32, grid_h=24, wrap=True),
    fadeout(4),
], clear=False))

FIXTURES["05-set-render-mode"] = preset(list_body([
    fadeout(20),
    set_render_mode(blend=1, alpha=255, width=2),    # additive, 2 px
    superscope("n=180;", "t=t+0.02;", "", WHEEL_POINT.replace("blue=0.2", "blue=0.9")),
    set_render_mode(blend=7, alpha=96, width=1),     # adjustable 96/255
    generic(14, 0),                                  # Ring
    generic(9, 0),                                   # Roto Blitter
]))

FIXTURES["06-buffer-save"] = preset(list_body([
    fadeout(30),
    superscope(SCOPE_INIT, SCOPE_FRAME, "", SCOPE_POINT),
    buffer_save(action=0, buf=0),                    # save
    generic(15, 4),                                  # Movement
    buffer_save(action=1, buf=0, blend=1, adjustable=128),   # restore additive
]))

FIXTURES["07-custom-bpm-onbeat"] = preset(list_body([
    custom_bpm(ms=400, skip=1),
    on_beat_clear(color=(8, 0, 24), blend=True, n_beats=2),
    superscope(SCOPE_INIT, SCOPE_FRAME, SCOPE_BEAT, SCOPE_POINT),
    moving_particle(color=(255, 200, 80)),
]))

FIXTURES["08-ape-components"] = preset(list_body([
    global_variables("reg00=0;", "reg00=reg00*0.9;", "reg00=1;"),
    superscope("n=120;", "", "", "x=i*2-1; y=reg00*0.5*sin(i*12); red=1; green=reg00; blue=0.3;"),
    ape("Winamp Brightness v1", 0, 0, 0),
    ape("Holden03: Convolution Filter", 0),
    ape("Color Map", 0, 0),
    ape("Winamp Mosaic v1", 0),
]))

FIXTURES["09-scripted-transforms"] = preset(list_body([
    fadeout(6),
    superscope(SCOPE_INIT, SCOPE_FRAME, SCOPE_BEAT, WHEEL_POINT),
    dynamic_shift("t=0;", DSHIFT_FRAME, "", blend=False, bilinear=True),
    dynamic_distance("dd=1;", "dd=1+(dd-1)*0.9;", "dd=1.15;", DDM_POINT),
    color_modifier("", "", "", CM_POINT, recompute=True),
]))

# Ignore-input nested list: nothing is copied in, so its buffer persists
# across frames and the scope drawn inside it accumulates through Movement —
# the classic trail-swirl construction (yay mk ii, pulsing shit, new taste).
FIXTURES["10-ignore-input-persistent-list"] = preset(list_body([
    fadeout(40),
    nested([superscope(SCOPE_INIT, SCOPE_FRAME, "", SCOPE_POINT),
            generic(15, 3), generic(22, 0)],
           in_blend=0, out_blend=1),
]))

FIXTURES["11-ancient-script-format"] = preset(list_body([
    superscope(SCOPE_INIT, SCOPE_FRAME, SCOPE_BEAT, SCOPE_POINT, ancient=True),
    blur(2),
], clear=True), magic=MAGIC01)

FIXTURES["12-color-and-geometry-registry"] = preset(list_body([
    superscope(SCOPE_INIT, SCOPE_FRAME, SCOPE_BEAT, WHEEL_POINT),
    generic(22, 0), generic(11, 0), generic(12, 0), generic(38, 0),
    invert(True), generic(41, 0), generic(26, 0), generic(30, 0),
    water(),
]))

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for name, data in FIXTURES.items():
        path = os.path.join(OUT, name + ".avs")
        with open(path, "wb") as f:
            f.write(data)
        print("%-40s %6d bytes" % (name + ".avs", len(data)))
