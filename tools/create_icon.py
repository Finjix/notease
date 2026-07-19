import math
import struct
from pathlib import Path


def pixel(size, x, y):
    scale = 4
    px = (x + 0.5) / size
    py = (y + 0.5) / size
    radius = 0.16
    edge = min(px, 1 - px, py, 1 - py)
    corner_distance = min(
        math.hypot(max(radius - px, 0, px - (1 - radius)),
                   max(radius - py, 0, py - (1 - radius))),
        1,
    )
    if px < radius and py < radius:
        distance = math.hypot(px - radius, py - radius)
    elif px > 1 - radius and py < radius:
        distance = math.hypot(px - (1 - radius), py - radius)
    elif px < radius and py > 1 - radius:
        distance = math.hypot(px - radius, py - (1 - radius))
    elif px > 1 - radius and py > 1 - radius:
        distance = math.hypot(px - (1 - radius), py - (1 - radius))
    else:
        distance = 0
    if distance > radius:
        return (0, 0, 0, 0)
    if py < 0.30:
        color = (248, 222, 116, 255)
    else:
        color = (255, 252, 232, 255)
    if edge < 0.012:
        color = (211, 180, 65, 255)
    return color


def dib(size):
    header = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32,
                         0, size * size * 4, 0, 0, 0, 0)
    data = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            r, g, b, a = pixel(size, x, y)
            data += bytes((b, g, r, a))
    mask = b'\0' * (((size + 31) // 32) * 4 * size)
    return header + data + mask


def main():
    sizes = (16, 32, 48, 64, 128, 256)
    images = [dib(size) for size in sizes]
    header = struct.pack('<HHH', 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = bytearray()
    for size, image in zip(sizes, images):
        dimension = 0 if size == 256 else size
        entries += struct.pack('<BBBBHHII', dimension, dimension, 0, 0, 1, 32,
                               len(image), offset)
        offset += len(image)
    Path('src/notease.ico').write_bytes(header + entries + b''.join(images))


if __name__ == '__main__':
    main()
