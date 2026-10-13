"""Pixel diff of two BMPs written by WriteBmpFile: count of differing pixels,
count > 2, and max channel difference. Usage: python scripts/perf/bmpdiff.py a.bmp b.bmp"""
import struct, sys

def load(p):
    b = open(p, 'rb').read()
    off = struct.unpack_from('<I', b, 10)[0]
    w, h = struct.unpack_from('<ii', b, 18)
    bpp = struct.unpack_from('<H', b, 28)[0]
    return b[off:], w, abs(h), bpp

a, wa, ha, ba = load(sys.argv[1])
b, wb, hb, bb = load(sys.argv[2])
assert (wa, ha, ba) == (wb, hb, bb), "size mismatch"
bpp = ba // 8
row = (wa * bpp + 3) & ~3
diff = big = 0
mx = 0
for y in range(ha):
    ra = a[y * row:y * row + wa * bpp]
    rb = b[y * row:y * row + wa * bpp]
    if ra == rb:
        continue
    for x in range(wa):
        pa = ra[x * bpp:x * bpp + 3]
        pb = rb[x * bpp:x * bpp + 3]
        if pa != pb:
            diff += 1
            d = max(abs(pa[i] - pb[i]) for i in range(3))
            mx = max(mx, d)
            if d > 2:
                big += 1
print(f"{sys.argv[1]} vs {sys.argv[2]}: {diff} px differ, {big} > 2, max {mx}")
