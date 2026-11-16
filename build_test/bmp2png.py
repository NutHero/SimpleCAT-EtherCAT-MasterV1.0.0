import struct, zlib, sys

def bmp2png(src, dst, crop=None, scale=1):
    with open(src, 'rb') as f:
        data = f.read()
    off = struct.unpack_from('<I', data, 10)[0]
    w, h = struct.unpack_from('<ii', data, 18)
    bpp = struct.unpack_from('<H', data, 28)[0]
    topdown = h < 0
    h = abs(h)
    rowsize = ((w * bpp + 31) // 32) * 4
    px = bytearray(w * h * 3)
    for y in range(h):
        sy = y if topdown else h - 1 - y
        row = data[off + sy * rowsize: off + sy * rowsize + w * 3]
        # BMP 行内是 BGR，转 RGB
        row = bytearray(row)
        row[0::3], row[2::3] = row[2::3], row[0::3]
        px[y * w * 3:(y + 1) * w * 3] = row
    if crop:
        x0, y0, x1, y1 = crop
        px2 = bytearray()
        for y in range(y0, y1):
            px2 += px[(y * w + x0) * 3:(y * w + x1) * 3]
        w, h, px = x1 - x0, y1 - y0, px2
    ow, oh = w * scale, h * scale
    if scale > 1:
        px2 = bytearray()
        for y in range(h):
            row = bytearray()
            for x in range(w):
                r, g, b = px[(y * w + x) * 3:(y * w + x) * 3 + 3]
                row += bytes([r, g, b] * scale)
            for _ in range(scale):
                px2 += row
        px = px2
        w, h = ow, oh
    raw = b''.join(b'\x00' + bytes(px[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    def chunk(t, d):
        c = t + d
        return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c))
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw))
           + chunk(b'IEND', b''))
    with open(dst, 'wb') as f:
        f.write(png)
    print(f'{dst}: {w}x{h}')

if __name__ == '__main__':
    src = sys.argv[1]
    dst = sys.argv[2]
    crop = tuple(int(v) for v in sys.argv[3].split(',')) if len(sys.argv) > 3 and sys.argv[3] != '-' else None
    scale = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    bmp2png(src, dst, crop, scale)
