#!/usr/bin/env python3
"""map_png.py -- a minimal PNG codec for the World map page's height / biome
import and export (docs/PLAN_map_v2.md, package E2 item 5). Standard library
only (zlib + struct): no Pillow, no numpy, because the tuner server ships as a
frozen exe and the codec must run wherever the server does.

What it WRITES:
  grey16  16-bit greyscale (colour type 0, depth 16) -- the height plane; what
          Gaea / World Machine / QGIS read and write as a heightmap.
  rgb8    8-bit RGB (colour type 2) -- the biome plane in palette colours.
  Both non-interlaced, every row filter 0 (None), zlib level 6, plus an
  optional tEXt chunk keyword "sandvox" carrying a JSON note (the page puts the
  mapping there -- metres per unit, offset, the post rectangle -- so an export
  re-imports onto the same posts with the same numbers).

What it READS: every non-interlaced PNG -- colour types 0 / 2 / 3 / 4 / 6 at
every legal depth, every row filter -- reduced to one of
  {"format": "grey", "max": 2**depth - 1, "samples": array('H')}   (types 0, 4)
  {"format": "rgb", "samples": bytes(w * h * 3)}                   (2, 3, 6)
Alpha is dropped; 16-bit colour keeps the high byte. Adam7-interlaced files
are refused with a message (no heightmap tool writes them by default).

SPEED: unfiltering is plain Python. Filter 0 and Up are fast; Sub / Average /
Paeth run ~0.3 us a byte, so a full 3137 x 3137 16-bit heightmap written with
Paeth rows takes ~10 s to read. That is an import, not a frame.

  python scripts/map_png.py --selftest      # the codec's round trips
  python scripts/map_png.py info <file.png> # what a file holds
"""
import json
import struct
import sys
import zlib
from array import array

PNG_SIG = b"\x89PNG\r\n\x1a\n"


def _chunk(kind, data):
    c = struct.pack(">I", len(data)) + kind + data
    return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def _text_chunk(note):
    if note is None:
        return b""
    s = note if isinstance(note, str) else json.dumps(note, separators=(",", ":"))
    return _chunk(b"tEXt", b"sandvox\x00" + s.encode("latin-1", "replace"))


def encode_grey16(w, h, samples, note=None):
    """A 16-bit greyscale PNG of w x h samples (row-major ints 0..65535)."""
    if len(samples) != w * h:
        raise ValueError("grey16: %d samples for %dx%d" % (len(samples), w, h))
    a = array("H", samples)
    if sys.byteorder == "little":
        a.byteswap()                      # PNG is big-endian
    raw = a.tobytes()
    stride = w * 2
    rows = bytearray()
    for y in range(h):
        rows += b"\x00"
        rows += raw[y * stride:(y + 1) * stride]
    return (PNG_SIG + _chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 16, 0, 0, 0, 0)) + _text_chunk(note)
            + _chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + _chunk(b"IEND", b""))


def encode_rgb8(w, h, rgb, note=None):
    """An 8-bit RGB PNG of w x h pixels (bytes, 3 per pixel, row-major)."""
    if len(rgb) != w * h * 3:
        raise ValueError("rgb8: %d bytes for %dx%d" % (len(rgb), w, h))
    stride = w * 3
    rows = bytearray()
    for y in range(h):
        rows += b"\x00"
        rows += rgb[y * stride:(y + 1) * stride]
    return (PNG_SIG + _chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + _text_chunk(note)
            + _chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + _chunk(b"IEND", b""))


_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
_DEPTHS = {0: (1, 2, 4, 8, 16), 2: (8, 16), 3: (1, 2, 4, 8), 4: (8, 16), 6: (8, 16)}


def _unfilter(data, w, h, bpp, stride):
    """The raw scanlines (no filter bytes) of a decompressed IDAT stream."""
    out = bytearray(h * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        if pos >= len(data):
            raise ValueError("image data ends early (row %d of %d)" % (y, h))
        ft = data[pos]
        row = bytearray(data[pos + 1:pos + 1 + stride])
        if len(row) != stride:
            raise ValueError("image data ends early (row %d of %d)" % (y, h))
        pos += 1 + stride
        if ft == 0:
            pass
        elif ft == 1:                                    # Sub
            for i in range(bpp, stride):
                row[i] = (row[i] + row[i - bpp]) & 255
        elif ft == 2:                                    # Up
            row = bytearray((a + b) & 255 for a, b in zip(row, prev))
        elif ft == 3:                                    # Average
            for i in range(stride):
                left = row[i - bpp] if i >= bpp else 0
                row[i] = (row[i] + ((left + prev[i]) >> 1)) & 255
        elif ft == 4:                                    # Paeth
            for i in range(stride):
                a = row[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                row[i] = (row[i] + pr) & 255
        else:
            raise ValueError("unknown row filter %d (row %d)" % (ft, y))
        out[y * stride:(y + 1) * stride] = row
        prev = row
    return out


def _unpack_bits(raw, w, h, depth, stride):
    """Sub-byte samples (depth 1 / 2 / 4) to one value per sample."""
    vals = array("H")
    mask = (1 << depth) - 1
    per = 8 // depth
    for y in range(h):
        base = y * stride
        for x in range(w):
            byte = raw[base + x // per]
            shift = 8 - depth * (x % per + 1)
            vals.append((byte >> shift) & mask)
    return vals


def decode(data):
    """Parse a PNG; see the module docstring for the result's shape."""
    if data[:8] != PNG_SIG:
        raise ValueError("not a PNG file")
    pos = 8
    ihdr = None
    idat = bytearray()
    plte = None
    text = {}
    while pos + 8 <= len(data):
        ln, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + ln]
        crc = data[pos + 8 + ln:pos + 12 + ln]
        if len(body) != ln or len(crc) != 4:
            raise ValueError("truncated %s chunk" % kind.decode("latin-1"))
        if struct.unpack(">I", crc)[0] != zlib.crc32(kind + body) & 0xFFFFFFFF:
            raise ValueError("%s chunk CRC mismatch (corrupt file)" % kind.decode("latin-1"))
        pos += 12 + ln
        if kind == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            plte = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"tEXt":
            k, _, v = body.partition(b"\x00")
            text[k.decode("latin-1")] = v.decode("latin-1")
        elif kind == b"IEND":
            break
    if not ihdr:
        raise ValueError("no IHDR chunk")
    w, h, depth, ctype, comp, filt, inter = ihdr
    if ctype not in _CHANNELS or depth not in _DEPTHS[ctype]:
        raise ValueError("unsupported colour type %d at depth %d" % (ctype, depth))
    if comp != 0 or filt != 0:
        raise ValueError("unknown compression / filter method")
    if inter != 0:
        raise ValueError("interlaced (Adam7) PNGs are not supported -- re-export without interlacing")
    if w <= 0 or h <= 0 or w > 16384 or h > 16384:
        raise ValueError("image is %dx%d (1..16384 a side)" % (w, h))
    ch = _CHANNELS[ctype]
    bits = ch * depth
    stride = (w * bits + 7) // 8
    bpp = max(1, bits // 8)
    raw = _unfilter(zlib.decompress(bytes(idat)), w, h, bpp, stride)
    out = {"w": w, "h": h, "depth": depth, "colourType": ctype, "text": text}
    n = w * h
    if ctype in (0, 4):                      # grey (+ alpha, dropped)
        if depth < 8:
            vals = _unpack_bits(raw, w, h, depth, stride)
        elif depth == 8:
            vals = array("H", iter(raw[0::ch]))  # an iterator: a bytes-like would be read as raw u16s
        else:
            a = array("H", bytes(raw))
            if sys.byteorder == "little":
                a.byteswap()
            vals = a[0::ch] if ch > 1 else a
        out.update(format="grey", max=(1 << depth) - 1, samples=vals)
        return out
    if ctype == 3:                           # palette -> rgb
        if not plte:
            raise ValueError("palette image without a PLTE chunk")
        idx = _unpack_bits(raw, w, h, depth, stride) if depth < 8 else raw
        rgb = bytearray(n * 3)
        for i in range(n):
            k = idx[i] * 3
            if k + 3 > len(plte):
                raise ValueError("palette index %d past the PLTE chunk" % idx[i])
            rgb[i * 3:i * 3 + 3] = plte[k:k + 3]
        out.update(format="rgb", samples=bytes(rgb))
        return out
    # rgb / rgba, 8 or 16 bit
    step = ch * (depth // 8)
    hi = depth // 8                          # 16-bit: keep the high (first) byte
    rgb = bytearray(n * 3)
    rgb[0::3] = raw[0::step]
    rgb[1::3] = raw[hi::step]
    rgb[2::3] = raw[2 * hi::step]
    out.update(format="rgb", samples=bytes(rgb))
    return out


def decoded_blob(d):
    """The tuner route's binary answer for a decoded PNG: 'SVPN', u32 w,
    u32 h, u32 format (1 grey, 2 rgb), u32 max, u32 noteLen, the note (UTF-8
    JSON of the tEXt chunks) padded to 4, then the samples (grey: u16 LE each;
    rgb: 3 bytes each)."""
    note = json.dumps(d["text"]).encode("utf-8")
    note += b"\x00" * ((-len(note)) % 4)
    if d["format"] == "grey":
        a = array("H", d["samples"])
        if sys.byteorder != "little":
            a.byteswap()
        body = a.tobytes()
        fmt, mx = 1, d["max"]
    else:
        body = d["samples"]
        fmt, mx = 2, 255
    return b"SVPN" + struct.pack("<IIIII", d["w"], d["h"], fmt, mx, len(note)) + note + body


def _selftest():
    import random
    fails = 0

    def ok(c, m):
        nonlocal fails
        print(("PASS  " if c else "FAIL  ") + m)
        if not c:
            fails += 1

    rnd = random.Random(7)
    w, h = 37, 23
    g = [rnd.randrange(65536) for _ in range(w * h)]
    png = encode_grey16(w, h, g, {"mpu": 0.1, "offsetM": -60})
    d = decode(png)
    ok(d["format"] == "grey" and d["max"] == 65535 and list(d["samples"]) == g, "grey16 round-trips exactly (%dx%d)" % (w, h))
    ok(json.loads(d["text"]["sandvox"]) == {"mpu": 0.1, "offsetM": -60}, "the sandvox tEXt note round-trips")
    rgb = bytes(rnd.randrange(256) for _ in range(w * h * 3))
    d = decode(encode_rgb8(w, h, rgb))
    ok(d["format"] == "rgb" and d["samples"] == rgb, "rgb8 round-trips exactly")

    # every filter type, through a hand-built 16-bit grey file
    def filtered(w, h, raw, bpp, stride, ft):
        out = bytearray()
        prev = bytearray(stride)
        for y in range(h):
            row = raw[y * stride:(y + 1) * stride]
            enc = bytearray(stride)
            for i in range(stride):
                a = row[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                if ft == 0:
                    pr = 0
                elif ft == 1:
                    pr = a
                elif ft == 2:
                    pr = b
                elif ft == 3:
                    pr = (a + b) >> 1
                else:
                    p = a + b - c
                    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                    pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                enc[i] = (row[i] - pr) & 255
            out += bytes([ft]) + enc
            prev = row
        return bytes(out)
    a = array("H", g)
    if sys.byteorder == "little":
        a.byteswap()
    raw = a.tobytes()
    for ft in range(5):
        body = filtered(w, h, raw, 2, w * 2, ft)
        png = (PNG_SIG + _chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 16, 0, 0, 0, 0))
               + _chunk(b"IDAT", zlib.compress(body)) + _chunk(b"IEND", b""))
        ok(list(decode(png)["samples"]) == g, "16-bit grey with every row filter %d decodes exactly" % ft)
    # 8-bit grey, 4-bit grey, a palette image, RGBA 16
    g8 = bytes(rnd.randrange(256) for _ in range(w * h))
    body = b"".join(b"\x00" + g8[y * w:(y + 1) * w] for y in range(h))
    png = PNG_SIG + _chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)) + _chunk(b"IDAT", zlib.compress(body)) + _chunk(b"IEND", b"")
    d = decode(png)
    ok(d["max"] == 255 and list(d["samples"]) == list(g8), "8-bit grey decodes (max 255)")
    g4 = [rnd.randrange(16) for _ in range(w * h)]
    stride = (w * 4 + 7) // 8
    rows = bytearray()
    for y in range(h):
        rows += b"\x00"
        r = bytearray(stride)
        for x in range(w):
            r[x // 2] |= g4[y * w + x] << (4 if x % 2 == 0 else 0)
        rows += r
    png = PNG_SIG + _chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 4, 0, 0, 0, 0)) + _chunk(b"IDAT", zlib.compress(bytes(rows))) + _chunk(b"IEND", b"")
    ok(list(decode(png)["samples"]) == g4, "4-bit grey decodes")
    pal = bytes([10, 20, 30, 200, 100, 50, 0, 255, 0])
    idx = bytes(rnd.randrange(3) for _ in range(w * h))
    body = b"".join(b"\x00" + idx[y * w:(y + 1) * w] for y in range(h))
    png = (PNG_SIG + _chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0)) + _chunk(b"PLTE", pal)
           + _chunk(b"IDAT", zlib.compress(body)) + _chunk(b"IEND", b""))
    d = decode(png)
    ok(d["format"] == "rgb" and all(d["samples"][i * 3:i * 3 + 3] == pal[idx[i] * 3:idx[i] * 3 + 3] for i in range(w * h)),
       "an indexed (palette) PNG decodes to its palette colours")
    rgba = bytearray()
    for i in range(w * h):
        rgba += bytes([rgb[i * 3], 7, rgb[i * 3 + 1], 7, rgb[i * 3 + 2], 7, 255, 255])
    body = b"".join(b"\x00" + bytes(rgba[y * w * 8:(y + 1) * w * 8]) for y in range(h))
    png = PNG_SIG + _chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 16, 6, 0, 0, 0)) + _chunk(b"IDAT", zlib.compress(body)) + _chunk(b"IEND", b"")
    ok(decode(png)["samples"] == rgb, "16-bit RGBA keeps the high bytes and drops alpha")
    bad = bytearray(encode_grey16(w, h, g))
    bad[40] ^= 0xFF
    try:
        decode(bytes(bad))
        ok(False, "a corrupt chunk is refused")
    except ValueError as e:
        ok("CRC" in str(e), "a corrupt chunk is refused: %s" % e)
    il = PNG_SIG + _chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 0, 0, 0, 1)) + _chunk(b"IEND", b"")
    try:
        decode(il)
        ok(False, "an interlaced file is refused")
    except ValueError as e:
        ok("interlaced" in str(e), "an interlaced file is refused: %s" % e)
    blob = decoded_blob(decode(encode_grey16(3, 2, [0, 1, 2, 65535, 4, 5], {"a": 1})))
    mg, bw, bh, fmt, mx, nl = struct.unpack("<4sIIIII", blob[:24])
    vals = array("H", blob[24 + nl:])
    ok(mg == b"SVPN" and (bw, bh, fmt, mx) == (3, 2, 1, 65535) and list(vals) == [0, 1, 2, 65535, 4, 5],
       "the route's SVPN blob carries size, format and the samples")
    print("map_png selftest: " + ("OK" if not fails else "%d FAILED" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        sys.exit(_selftest())
    if len(sys.argv) > 2 and sys.argv[1] == "info":
        with open(sys.argv[2], "rb") as f:
            d = decode(f.read())
        s = d["samples"]
        rng = (min(s), max(s)) if d["format"] == "grey" else None
        print("%dx%d colour type %d depth %d -> %s%s; text %s" % (
            d["w"], d["h"], d["colourType"], d["depth"], d["format"],
            (" values %d..%d of %d" % (rng[0], rng[1], d["max"])) if rng else "", d["text"]))
        sys.exit(0)
    print(__doc__)
    sys.exit(2)
