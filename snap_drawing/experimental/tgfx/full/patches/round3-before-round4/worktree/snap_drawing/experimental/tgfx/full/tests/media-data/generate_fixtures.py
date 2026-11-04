#!/usr/bin/env python3
"""Author the tiny format fixtures; never imports or runs the production decoder.

The patterns, JSON, SVG, PNG/APNG chunk assembly, and simple GIF encoding here
were authored for these checks and are covered by the repository MIT license.
Pillow is used only as an independent JPEG/VP8L encoder. The C++ checks do not
need Python or Pillow. Noto Sans and Montserrat and their OFL notices are copied
separately and deliberately are not regenerated or subsetted by this script.
"""
from pathlib import Path
import base64
import hashlib
import io
import json
import struct
import zlib
from PIL import Image, __version__ as pillow_version

ROOT = Path(__file__).resolve().parent
RED = (255, 0, 0, 255)
GREEN = (0, 255, 0, 255)
BLUE = (0, 0, 255, 255)
WHITE = (255, 255, 255, 255)
HALF_WHITE = (255, 255, 255, 128)


def png_chunk(name, body):
    name = name.encode("ascii")
    return struct.pack(">I", len(body)) + name + body + struct.pack(">I", zlib.crc32(name + body))


def scanlines(width, height, pixels):
    return zlib.compress(b"".join(b"\0" + bytes(pixels[y * width * 4:(y + 1) * width * 4])
                                 for y in range(height)), level=9)


def ihdr(width, height):
    return struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)


signature = b"\x89PNG\r\n\x1a\n"
pattern = bytes(RED + GREEN + BLUE + WHITE)
png = signature + png_chunk("IHDR", ihdr(2, 2)) + png_chunk("IDAT", scanlines(2, 2, pattern))
png += png_chunk("IEND", b"")
(ROOT / "pattern.png").write_bytes(png)
(ROOT / "truncated.png").write_bytes(png[:24])
Image.new("RGB", (16, 16), (200, 40, 80)).save(ROOT / "solid.jpg", quality=100, subsampling=0)
(ROOT / "truncated.jpg").write_bytes((ROOT / "solid.jpg").read_bytes()[:20])


def jpeg_segments(encoded):
    """Read the pre-SOS marker envelope, without decoding JPEG pixels."""
    assert encoded[:2] == b"\xff\xd8"
    offset = 2
    result = []
    while offset + 4 <= len(encoded):
        assert encoded[offset] == 0xff
        marker = encoded[offset + 1]
        size = struct.unpack_from(">H", encoded, offset + 2)[0]
        end = offset + 2 + size
        assert size >= 2 and end <= len(encoded)
        result.append((marker, offset, end))
        if marker == 0xda:
            return result, end
        offset = end
    raise RuntimeError("Independent JPEG fixture has no complete SOS header")


solid_jpeg = (ROOT / "solid.jpg").read_bytes()
segments, entropy_start = jpeg_segments(solid_jpeg)
(ROOT / "jpeg-truncated-after-sos.jpg").write_bytes(solid_jpeg[:entropy_start])
(ROOT / "jpeg-missing-eoi.jpg").write_bytes(solid_jpeg[:-2])
# A complete SOF/SOS with absent DQT is a real start_decompress error
# (JERR_NO_QUANT_TABLE), rather than an EOF-warning-only test.
without_quantization = b"\xff\xd8" + b"".join(solid_jpeg[start:end] for marker, start, end in segments
                                            if marker != 0xdb) + solid_jpeg[entropy_start:]
(ROOT / "jpeg-missing-quant-tables.jpg").write_bytes(without_quantization)
scanline_pattern = Image.frombytes("RGB", (64, 64), bytes(
    component for y in range(64) for x in range(64)
    for component in ((x * 37 + y * 11) % 256, (x * 7 + y * 43) % 256, (x * 53 + y * 3) % 256)))
scanline_stream = io.BytesIO()
scanline_pattern.save(scanline_stream, format="JPEG", quality=95, subsampling=0)
scanline_jpeg = scanline_stream.getvalue()
_, scanline_start = jpeg_segments(scanline_jpeg)
cut = scanline_start + (len(scanline_jpeg) - scanline_start - 2) * 3 // 5
(ROOT / "jpeg-truncated-during-scanlines.jpg").write_bytes(scanline_jpeg[:cut])
webp_pattern = Image.frombytes("RGBA", (2, 2), bytes(RED + GREEN + BLUE + HALF_WHITE))
webp_pattern.save(ROOT / "pattern.webp", lossless=True, exact=True, quality=100)
(ROOT / "truncated-still.webp").write_bytes((ROOT / "pattern.webp").read_bytes()[:16])

# APNG: actual subrectangles, both disposal operations, SOURCE and OVER.
# Frame 0 is included in the animation. All unmentioned pixels retain the
# canvas state established by the format's blend/disposal rules.
apng = signature + png_chunk("IHDR", ihdr(4, 4))
apng += png_chunk("acTL", struct.pack(">II", 4, 0))
sequence = 0
for index, (x, y, w, h, color, delay, dispose, blend) in enumerate([
    (0, 0, 4, 4, RED, 100, 0, 0),
    (0, 0, 2, 2, GREEN, 200, 1, 0),
    (2, 2, 2, 2, BLUE, 300, 2, 0),
    (2, 0, 2, 2, HALF_WHITE, 400, 0, 1),
]):
    control = struct.pack(">IIIIIHHBB", sequence, w, h, x, y, delay, 1000, dispose, blend)
    apng += png_chunk("fcTL", control)
    sequence += 1
    payload = scanlines(w, h, bytes(color) * (w * h))
    if index == 0:
        apng += png_chunk("IDAT", payload)
    else:
        apng += png_chunk("fdAT", struct.pack(">I", sequence) + payload)
        sequence += 1
apng += png_chunk("IEND", b"")
(ROOT / "disposal.apng").write_bytes(apng)
# Deliberately corrupt a CRC, keeping the container otherwise complete.
bad_crc = bytearray(apng)
bad_crc[-1] ^= 1
(ROOT / "bad-crc.apng").write_bytes(bad_crc)
(ROOT / "truncated.apng").write_bytes(apng[:-14])

# Poster is an ordinary IDAT not preceded by fcTL: it must not become a
# synthesized animation frame. The static image API should still show it.
poster = signature + png_chunk("IHDR", ihdr(4, 4)) + png_chunk("acTL", struct.pack(">II", 2, 0))
poster += png_chunk("IDAT", scanlines(4, 4, bytes((255, 255, 0, 255)) * 16))
for seq, x, color, delay in [(0, 0, BLUE, 100), (2, 2, RED, 200)]:
    poster += png_chunk("fcTL", struct.pack(">IIIIIHHBB", seq, 2, 2, x, 0, delay, 1000, 0, 0))
    poster += png_chunk("fdAT", struct.pack(">I", seq + 1) + scanlines(2, 2, bytes(color) * 4))
poster += png_chunk("IEND", b"")
(ROOT / "poster.apng").write_bytes(poster)

# GIF89a, eight-entry global palette. Reset before every literal so no LZW
# dictionary expansion is needed. This is an encoder for a tiny fixture, not
# a second implementation of the production decoder.
palette = bytes((0, 0, 0) + RED[:3] + GREEN[:3] + BLUE[:3] + WHITE[:3]) + bytes(9)
gif = b"GIF89a" + struct.pack("<HHBBB", 4, 4, 0xF2, 0, 0) + palette
gif += b"\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00"
for x, y, w, h, color_index, delay_cs, disposal in [
    (0, 0, 4, 4, 1, 10, 1),
    (0, 0, 2, 2, 2, 20, 2),
    (2, 2, 2, 2, 3, 30, 3),
    (2, 0, 2, 2, 4, 40, 1),
]:
    gif += b"\x21\xf9\x04" + struct.pack("<BHB", (disposal << 2) | 1, delay_cs, 0) + b"\0"
    gif += b"\x2c" + struct.pack("<HHHHB", x, y, w, h, 0)
    codes = [v for _ in range(w * h) for v in (8, color_index)] + [9]
    packed = bytearray((len(codes) + 1) // 2)
    for i, code in enumerate(codes):
        packed[i // 2] |= code << (4 * (i % 2))
    gif += b"\x03" + bytes((len(packed),)) + packed + b"\0"
gif += b"\x3b"
(ROOT / "disposal.gif").write_bytes(gif)
(ROOT / "truncated.gif").write_bytes(gif[:18])
interlaced = b"GIF89a" + struct.pack("<HHBBB", 4, 4, 0xF2, 0, 0) + palette
interlaced += b"\x2c" + struct.pack("<HHHHB", 0, 0, 4, 4, 0x40)
codes = [v for row in (1, 3, 2, 4) for _ in range(4) for v in (8, row)] + [9]
packed = bytearray((len(codes) + 1) // 2)
for i, code in enumerate(codes):
    packed[i // 2] |= code << (4 * (i % 2))
interlaced += b"\x03" + bytes((len(packed),)) + packed + b"\0\x3b"
(ROOT / "interlaced.gif").write_bytes(interlaced)


def u24(value):
    return value.to_bytes(3, "little")


def riff_chunk(name, payload):
    return name + struct.pack("<I", len(payload)) + payload + (b"\0" if len(payload) & 1 else b"")


def vp8l(width, height, rgba):
    stream = io.BytesIO()
    Image.new("RGBA", (width, height), rgba).save(stream, format="WEBP", lossless=True, exact=True)
    encoded = stream.getvalue()
    offset = 12
    while offset + 8 <= len(encoded):
        length = struct.unpack_from("<I", encoded, offset + 4)[0]
        if encoded[offset:offset + 4] == b"VP8L":
            return riff_chunk(b"VP8L", encoded[offset + 8:offset + 8 + length])
        offset += 8 + length + (length & 1)
    raise RuntimeError("Pillow did not emit the required independent VP8L payload")


# WebP extended container. ANMF bit 0=dispose to background, bit 1=no blend.
body = riff_chunk(b"VP8X", b"\x12\0\0\0" + u24(3) + u24(3))
body += riff_chunk(b"ANIM", struct.pack("<IH", 0, 0))
for x, y, w, h, color, delay, flags in [
    (0, 0, 4, 4, RED, 100, 2),
    (0, 0, 2, 2, GREEN, 200, 3),
    (2, 2, 2, 2, BLUE, 300, 2),
    (0, 0, 2, 2, HALF_WHITE, 400, 0),
]:
    frame = u24(x // 2) + u24(y // 2) + u24(w - 1) + u24(h - 1) + u24(delay) + bytes((flags,))
    body += riff_chunk(b"ANMF", frame + vp8l(w, h, color))
animated_webp = b"RIFF" + struct.pack("<I", len(body) + 4) + b"WEBP" + body
(ROOT / "disposal.webp").write_bytes(animated_webp)
(ROOT / "truncated.webp").write_bytes(animated_webp[:30])

ks = {"o": {"a": 0, "k": 100}, "r": {"a": 0, "k": 0}, "a": {"a": 0, "k": [0, 0, 0]},
      "s": {"a": 0, "k": [100, 100, 100]}}
moving_ks = dict(ks, p={"a": 1, "k": [{"t": 0, "s": [4, 4, 0], "h": 1},
                                    {"t": 10, "s": [20, 4, 0], "h": 1},
                                    {"t": 20, "s": [20, 4, 0]}]})
lottie = {"v": "5.7.4", "fr": 10, "ip": 0, "op": 20, "w": 32, "h": 16, "ddd": 0,
          "assets": [], "layers": [{"ddd": 0, "ind": 1, "ty": 1, "nm": "moving red square",
                                    "sr": 1, "sw": 8, "sh": 8, "sc": "#ff0000", "ks": moving_ks,
                                    "ip": 0, "op": 20, "st": 0, "bm": 0}]}
(ROOT / "moving-solid.json").write_text(json.dumps(lottie, separators=(",", ":")) + "\n")
text_ks = dict(ks, p={"a": 0, "k": [8, 44, 0]})
text = {"v": "5.7.4", "fr": 10, "ip": 0, "op": 20, "w": 128, "h": 64, "ddd": 0, "assets": [],
        "fonts": {"list": [{"fName": "ValdiMediaFixture", "fFamily": "ValdiMediaFixture",
                            "fStyle": "Regular", "ascent": 75}]},
        "layers": [{"ddd": 0, "ind": 1, "ty": 5, "nm": "custom font HgQ", "sr": 1, "ks": text_ks,
                    "t": {"d": {"k": [{"t": 0, "s": {"f": "ValdiMediaFixture", "s": 32, "t": "HgQ",
                                                    "j": 0, "tr": 0, "lh": 38, "ls": 0,
                                                    "fc": [1, 0, 0]}}]}},
                    "ip": 0, "op": 20, "st": 0, "bm": 0}]}
(ROOT / "custom-font.json").write_text(json.dumps(text, separators=(",", ":")) + "\n")
(ROOT / "pattern.svg").write_text(
    '<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8" viewBox="0 0 16 8">'
    '<rect width="8" height="8" fill="#ff0000"/>'
    '<rect x="8" width="8" height="8" fill="#0000ff"/></svg>\n')
(ROOT / "malformed.json").write_text('{"v":"5.7.4","layers":[\n')
(ROOT / "malformed.svg").write_text('<svg xmlns="http://www.w3.org/2000/svg"><path d="\n')
(ROOT / "not-image.bin").write_bytes(b"This is explicitly not an encoded image.\0")

# Capacity regressions. They declare expensive decoded canvases/timelines but
# encode tiny actual rectangles. Never construct their expanded animation in
# the fixture generator or use the production decoder.
def capacity_gif(width, height, count):
    encoded = b"GIF89a" + struct.pack("<HHBBB", width, height, 0x80, 0, 0)
    encoded += bytes((0, 0, 0, 255, 0, 0))  # Two-entry black/red global palette.
    # Real 1x1 LZW image: three 3-bit codes CLEAR=4, literal=1, STOP=5.
    frame = b"\x21\xf9\x04\x05\x0a\x00\x00\x00"
    frame += b"\x2c" + struct.pack("<HHHHB", 0, 0, 1, 1, 0)
    frame += b"\x02\x02\x4c\x01\x00"
    return encoded + frame * count + b"\x3b"


def capacity_webp(width, height, count):
    encoded = riff_chunk(b"VP8X", b"\x12\0\0\0" + u24(width - 1) + u24(height - 1))
    encoded += riff_chunk(b"ANIM", struct.pack("<IH", 0, 0))
    delta = vp8l(1, 1, RED)
    frame = u24(0) + u24(0) + u24(0) + u24(0) + u24(100) + b"\x02" + delta
    encoded += riff_chunk(b"ANMF", frame) * count
    return b"RIFF" + struct.pack("<I", len(encoded) + 4) + b"WEBP" + encoded


def capacity_apng(width, height, count, declared=None):
    encoded = signature + png_chunk("IHDR", ihdr(width, height))
    encoded += png_chunk("acTL", struct.pack(">II", count if declared is None else declared, 0))
    # A genuine transparent default poster, excluded from the animation. Its
    # largest generator-side raw span is only 4 MB (the 1000x1000 case).
    encoded += png_chunk("IDAT", scanlines(width, height, bytes(width * height * 4)))
    red_pixel = scanlines(1, 1, bytes(RED))
    for index in range(count):
        encoded += png_chunk("fcTL", struct.pack(">IIIIIHHBB", index * 2, 1, 1, 0, 0, 100, 1000, 0, 0))
        encoded += png_chunk("fdAT", struct.pack(">I", index * 2 + 1) + red_pixel)
    return encoded + png_chunk("IEND", b"")


for extension, encoder in (("gif", capacity_gif), ("webp", capacity_webp), ("apng", capacity_apng)):
    (ROOT / f"capacity-delta-timeline.{extension}").write_bytes(encoder(1000, 1000, 100))
    (ROOT / f"capacity-frame-count.{extension}").write_bytes(encoder(1, 1, 4097))
    (ROOT / f"capacity-small-control.{extension}").write_bytes(encoder(4, 4, 3))

(ROOT / "capacity-canvas.gif").write_bytes(capacity_gif(20000, 20000, 1))
(ROOT / "capacity-canvas.webp").write_bytes(capacity_webp(20000, 20000, 1))
# Deliberately header-only: the CRC-valid IHDR/acTL alone already exceeds the
# budget, before an IDAT or its decompression may be attempted.
(ROOT / "capacity-canvas.apng").write_bytes(
    signature + png_chunk("IHDR", ihdr(20000, 20000)) + png_chunk("acTL", struct.pack(">II", 2, 0)))
(ROOT / "capacity-extra-frames.apng").write_bytes(capacity_apng(4, 4, 3, declared=1))
(ROOT / "large-intrinsic.svg").write_text(
    '<svg xmlns="http://www.w3.org/2000/svg" width="20000" height="20000" viewBox="0 0 20000 20000">'
    '<rect width="20000" height="20000" fill="#ff0000"/></svg>\n')
(ROOT / "svg-arial-text.svg").write_text(
    '<svg xmlns="http://www.w3.org/2000/svg" width="128" height="64">'
    '<text x="8" y="44" font-family="Arial" font-size="32" fill="#ff0000">Hello</text></svg>\n')
(ROOT / "svg-owned-text.svg").write_text(
    '<svg xmlns="http://www.w3.org/2000/svg" width="128" height="64">'
    '<text x="8" y="44" font-family="ValdiMediaFixture" font-size="32" fill="#ff0000">HgQ</text></svg>\n')


def nested_svg(child):
    # Author a genuine SVG image element containing an independently valid SVG.
    # No production XML or font resolver is used to create these envelopes.
    uri = "data:image/svg+xml;base64," + base64.b64encode(child).decode("ascii")
    return ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
            'width="128" height="64"><image x="0" y="0" width="128" height="64" '
            f'xlink:href="{uri}"/></svg>\n').encode("utf-8")


nested_arial = nested_svg((ROOT / "svg-arial-text.svg").read_bytes())
(ROOT / "svg-nested-arial.svg").write_bytes(nested_arial)
(ROOT / "svg-nested-twice-arial.svg").write_bytes(nested_svg(nested_arial))
(ROOT / "svg-nested-owned.svg").write_bytes(nested_svg((ROOT / "svg-owned-text.svg").read_bytes()))
# The embedded bytes are the unchanged, separately licensed Noto fixture.
# This document therefore also carries the Noto SIL OFL, not just author MIT.
embedded_font = base64.b64encode((ROOT / "NotoSans-Regular.ttf").read_bytes()).decode("ascii")
(ROOT / "svg-embedded-font.svg").write_text(
    '<svg xmlns="http://www.w3.org/2000/svg" width="128" height="64"><style>'
    '@font-face{font-family:ValdiEmbeddedFixture;src:url(data:font/ttf;base64,' +
    embedded_font + ');}</style><text x="8" y="44" font-family="ValdiEmbeddedFixture" '
    'font-size="32" fill="#ff0000">HgQ</text></svg>\n')
(ROOT / "sharp-detail.svg").write_text(
    '<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">' +
    ''.join(f'<rect x="{2 * index + 0.25}" width="1" height="100" fill="{color}"/>'
            for index in range(50) for color in ("#ff0000" if index % 2 == 0 else "#0000ff",)) +
    '</svg>\n')
detail_layers = []
for index in range(50):
    start = 2 * index + 0.25
    transform = dict(ks, p={"a": 1, "k": [
        {"t": 0, "s": [start, 0, 0], "h": 1},
        {"t": 10, "s": [start + 0.5, 0, 0], "h": 1},
        {"t": 20, "s": [start + 0.5, 0, 0]}]})
    detail_layers.append({"ddd": 0, "ind": index + 1, "ty": 1, "nm": f"stripe {index}", "sr": 1,
                          "sw": 1, "sh": 100, "sc": "#ff0000" if index % 2 == 0 else "#0000ff",
                          "ks": transform, "ip": 0, "op": 20, "st": 0, "bm": 0})
detail_lottie = {"v": "5.7.4", "fr": 10, "ip": 0, "op": 20, "w": 100, "h": 100, "ddd": 0,
                 "assets": [], "layers": detail_layers}
(ROOT / "sharp-detail.json").write_text(json.dumps(detail_lottie, separators=(",", ":")) + "\n")
manifest = {"authored_fixture_license": "Repository MIT", "independent_pillow_encoder_version": pillow_version,
            "fixture_license_overrides": {"svg-embedded-font.svg": "Repository MIT XML envelope; SIL OFL 1.1 embedded Noto Sans, see NotoSans-LICENSE.txt"},
            "files": {p.name: {"bytes": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                      for p in sorted(ROOT.iterdir()) if (p.suffix in {".png", ".jpg", ".webp", ".gif", ".apng",
                                                                       ".svg", ".json", ".bin", ".ttf"}
                                                         or p.name.endswith("-LICENSE.txt"))
                      and p.name != "manifest.json"}}
(ROOT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
