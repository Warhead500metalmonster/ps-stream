# Draws app.ico and the 256 px preview: python -I make_icon.py (standard library only)
# The mark is a Wi-Fi sign whose dot is a play button, since the app is built for weak Wi-Fi.
import math, os, struct, zlib

NIGHT_TOP, NIGHT_BOTTOM = (40, 36, 98), (10, 12, 36)
CYAN, VIOLET = (54, 214, 255), (150, 92, 255)
WHITE = (255, 255, 255)
CENTER = (0.5, 0.735)          # where the Wi-Fi arcs come from
ARCS = (0.205, 0.335, 0.465)   # radii, inner to outer
ARC_WIDTH = 0.072
ARC_HALF_ANGLE = math.radians(44)
# Play button in place of the dot, slightly right of center so it looks centered
TRIANGLE = ((0.445, 0.652), (0.445, 0.818), (0.588, 0.735))
TRIANGLE_ROUND = 0.018

def lerp(a, b, t):
    t = min(max(t, 0.0), 1.0)
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))

def inside_rounded(x, y, r):
    dx = max(abs(x - 0.5) - (0.5 - r), 0)
    dy = max(abs(y - 0.5) - (0.5 - r), 0)
    return dx * dx + dy * dy <= r * r

def segment_distance(px, py, ax, ay, bx, by):
    vx, vy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy)))
    return math.hypot(px - ax - t * vx, py - ay - t * vy)

def triangle_distance(x, y):
    (ax, ay), (bx, by), (cx, cy) = TRIANGLE
    def side(px, py, qx, qy):
        return (qx - px) * (y - py) - (qy - py) * (x - px)
    s1, s2, s3 = side(ax, ay, bx, by), side(bx, by, cx, cy), side(cx, cy, ax, ay)
    if (s1 <= 0 and s2 <= 0 and s3 <= 0) or (s1 >= 0 and s2 >= 0 and s3 >= 0):
        return 0.0
    return min(segment_distance(x, y, ax, ay, bx, by), segment_distance(x, y, bx, by, cx, cy),
               segment_distance(x, y, cx, cy, ax, ay))

def arc_distance(x, y, radius):
    # Distance to an arc opening upward, with round ends
    dx, dy = x - CENTER[0], CENTER[1] - y
    angle = math.atan2(dx, dy)  # 0 straight up
    if abs(angle) <= ARC_HALF_ANGLE:
        return abs(math.hypot(dx, dy) - radius)
    end = math.copysign(ARC_HALF_ANGLE, angle)
    ex, ey = CENTER[0] + radius * math.sin(end), CENTER[1] - radius * math.cos(end)
    return math.hypot(x - ex, y - ey)

def color(x, y):
    if not inside_rounded(x, y, 0.22):
        return None
    if triangle_distance(x, y) <= TRIANGLE_ROUND:
        return WHITE
    for radius in ARCS:
        if arc_distance(x, y, radius) <= ARC_WIDTH / 2:
            return lerp(CYAN, VIOLET, (x - 0.18) / 0.64)
    base = lerp(NIGHT_TOP, NIGHT_BOTTOM, y)
    # A soft glow behind the sign
    glow = max(0.0, 1 - math.hypot(x - CENTER[0], (y - 0.55) * 1.2) / 0.55)
    return lerp(base, (70, 60, 170), glow * glow * 0.55)

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
open(os.path.join(here, '..', 'docs', 'icon.png'), 'wb').write(images[-1])
open(os.path.join(here, 'preview_32.png'), 'wb').write(render(32)) if os.environ.get('ICON_SMALL_PREVIEW') else None
print('ok')
