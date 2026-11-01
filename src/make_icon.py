# Draws app.ico: python -I make_icon.py (standard library only)
import math, os, struct, zlib

def lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))

def inside_rounded(x, y, r):
    dx = max(abs(x - 0.5) - (0.5 - r), 0)
    dy = max(abs(y - 0.5) - (0.5 - r), 0)
    return dx * dx + dy * dy <= r * r

def inside_triangle(x, y):
    # Play triangle with softly rounded corners
    ax, ay, bx, by, cx, cy = 0.20, 0.27, 0.20, 0.73, 0.55, 0.50
    def edge(px, py, qx, qy):
        length = math.hypot(qx - px, qy - py)
        return ((qx - px) * (y - py) - (qy - py) * (x - px)) / length
    r = 0.03
    return edge(ax, ay, bx, by) <= -r + 0.06 and edge(bx, by, cx, cy) <= -r + 0.06 and edge(cx, cy, ax, ay) <= -r + 0.06

def in_arc(x, y, radius, width):
    dx, dy = x - 0.46, y - 0.5
    if dx <= 0 or abs(math.atan2(dy, dx)) > 0.72:
        return False
    return abs(math.hypot(dx, dy) - radius) <= width / 2

def color(x, y):
    if not inside_rounded(x, y, 0.2):
        return None
    if inside_triangle(x, y):
        return (255, 255, 255)
    if in_arc(x, y, 0.20, 0.06) or in_arc(x, y, 0.33, 0.06):
        return (250, 176, 48)
    t = (x + y) / 2
    return lerp((58, 124, 255), (16, 34, 120), t)

def render(size):
    ss = 4
    rows = []
    for py in range(size):
        row = bytearray([0])
        for px in range(size):
            r = g = b = a = 0
            for sy in range(ss):
                for sx in range(ss):
                    c = color((px + (sx + 0.5) / ss) / size, (py + (sy + 0.5) / ss) / size)
                    if c:
                        r += c[0]; g += c[1]; b += c[2]; a += 1
            if a:
                row += bytes((round(r / a), round(g / a), round(b / a), round(255 * a / (ss * ss))))
            else:
                row += bytes(4)
        rows.append(bytes(row))
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) \
        + chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b'')

sizes = [16, 24, 32, 48, 64, 128, 256]
images = [render(s) for s in sizes]
offset = 6 + 16 * len(sizes)
out = struct.pack('<HHH', 0, 1, len(sizes))
for size, image in zip(sizes, images):
    out += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(image), offset)
    offset += len(image)
here = os.path.dirname(os.path.abspath(__file__))
open(os.path.join(here, 'app.ico'), 'wb').write(out + b''.join(images))
open(os.path.join(here, 'app_preview.png'), 'wb').write(images[-1])
print('ok')
