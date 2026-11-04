#!/usr/bin/env python3
"""Repack the two licensed source fonts into a real two-face TTC; no native tools."""
import hashlib
import json
from pathlib import Path
import struct

HERE = Path(__file__).resolve().parent


def records(data):
    count = struct.unpack_from(">H", data, 4)[0]
    result = {}
    for i in range(count):
        tag, _, offset, size = struct.unpack_from(">4sIII", data, 12 + i * 16)
        result[tag] = data[offset:offset + size]
        assert len(result[tag]) == size
    return result


def checksum(data):
    padded = data + b"\0" * (-len(data) % 4)
    return sum(struct.unpack(">" + "I" * (len(padded) // 4), padded)) & 0xffffffff


def glyph_id(font, character):
    """Independent source cmap parsing, used only to record fixed fixture expectations."""
    cmap = records(font)[b"cmap"]
    candidates = []
    for i in range(struct.unpack_from(">H", cmap, 2)[0]):
        platform, encoding, offset = struct.unpack_from(">HHI", cmap, 4 + 8 * i)
        if platform == 0 or (platform == 3 and encoding in (1, 10)):
            candidates.append(offset)
    for offset in candidates:
        fmt = struct.unpack_from(">H", cmap, offset)[0]
        if fmt == 12:
            for i in range(struct.unpack_from(">I", cmap, offset + 12)[0]):
                start, end, glyph = struct.unpack_from(">III", cmap, offset + 16 + 12 * i)
                if start <= character <= end:
                    return glyph + character - start
        elif fmt == 4 and character <= 0xffff:
            segments = struct.unpack_from(">H", cmap, offset + 6)[0] // 2
            ends = offset + 14
            starts = ends + segments * 2 + 2
            deltas = starts + segments * 2
            ranges = deltas + segments * 2
            for i in range(segments):
                start = struct.unpack_from(">H", cmap, starts + 2 * i)[0]
                end = struct.unpack_from(">H", cmap, ends + 2 * i)[0]
                if start <= character <= end:
                    delta = struct.unpack_from(">h", cmap, deltas + 2 * i)[0]
                    distance = struct.unpack_from(">H", cmap, ranges + 2 * i)[0]
                    if distance == 0:
                        return (character + delta) & 0xffff
                    position = ranges + 2 * i + distance + 2 * (character - start)
                    glyph = struct.unpack_from(">H", cmap, position)[0]
                    return ((glyph + delta) & 0xffff) if glyph else 0
    return 0


def main():
    names = ["RobotoFlex.ttf", "NotoSans-Regular.ttf"]
    fonts = [(HERE / name).read_bytes() for name in names]
    tables = [records(font) for font in fonts]
    offsets = []
    cursor = 12 + 4 * len(fonts)
    for table_set in tables:
        offsets.append(cursor)
        cursor += 12 + 16 * len(table_set)
    output = bytearray(cursor)
    struct.pack_into(">4sII", output, 0, b"ttcf", 0x10000, len(fonts))
    for i, offset in enumerate(offsets):
        struct.pack_into(">I", output, 12 + i * 4, offset)
    for index, table_set in enumerate(tables):
        count = len(table_set)
        power = 1 << (count.bit_length() - 1)
        struct.pack_into(">4sHHHH", output, offsets[index], fonts[index][:4], count,
                         16 * power, power.bit_length() - 1, 16 * (count - power))
        for i, (tag, payload) in enumerate(sorted(table_set.items())):
            if tag == b"head":
                payload = payload[:8] + b"\0\0\0\0" + payload[12:]
            offset = len(output)
            output.extend(payload)
            output.extend(b"\0" * (-len(output) % 4))
            struct.pack_into(">4sIII", output, offsets[index] + 12 + 16 * i,
                             tag, checksum(payload), offset, len(payload))
    collection = HERE / "selected-face.ttc"
    collection.write_bytes(output)
    characters = "HgQaZ0\u0100\u03a9"
    manifest = {
        "format": "TrueType Collection version 1, two complete non-deduplicated faces",
        "sources": [{
            "file": name, "bytes": len(font), "sha256": hashlib.sha256(font).hexdigest(),
            "glyphs": {f"U+{ord(c):04X}": glyph_id(font, ord(c)) for c in characters}
        } for name, font in zip(names, fonts)],
        "collection": {
            "file": collection.name, "bytes": len(output),
            "sha256": hashlib.sha256(output).hexdigest(), "faceOffsets": offsets
        },
        "selectedFace": 1,
        "packing": "Preserve all source tables; relocate directory offsets and zero head.checkSumAdjustment per TTC convention",
    }
    (HERE / "collection-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
