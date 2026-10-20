# Copyright 2026 Kim2091 (Sparkles)
# SPDX-License-Identifier: Apache-2.0

"""Minimal OpenEXR reader for scanline images with ZIP/ZIPS compression and HALF/FLOAT channels,
enough for the NASA SVS star maps (no dependency beyond numpy + zlib).

    rgb = read_exr('milkyway_2020_4k.exr')   # float32 (height, width, 3), R G B
"""
import struct
import zlib

import numpy as np


def _read_header(f):
    attrs = {}
    while True:
        name = b''
        while (c := f.read(1)) != b'\0':
            name += c
        if not name:
            return attrs
        atype = b''
        while (c := f.read(1)) != b'\0':
            atype += c
        size = struct.unpack('<i', f.read(4))[0]
        attrs[name.decode()] = (atype.decode(), f.read(size))


def _channels(data):
    out, i = [], 0
    while data[i:i + 1] != b'\0':
        j = data.index(b'\0', i)
        name = data[i:j].decode()
        ptype = struct.unpack('<i', data[j + 1:j + 5])[0]
        out.append((name, ptype))
        i = j + 1 + 16
    return out


def read_exr(path):
    with open(path, 'rb') as f:
        magic, version = struct.unpack('<ii', f.read(8))
        assert magic == 20000630, 'not an OpenEXR file'
        attrs = _read_header(f)
        chans = _channels(attrs['channels'][1])
        compression = attrs['compression'][1][0]
        x0, y0, x1, y1 = struct.unpack('<iiii', attrs['dataWindow'][1])
        w, h = x1 - x0 + 1, y1 - y0 + 1
        lines = {2: 1, 3: 16}[compression]  # ZIPS, ZIP
        nchunks = (h + lines - 1) // lines
        offsets = struct.unpack('<%dQ' % nchunks, f.read(8 * nchunks))
        sizes = {1: 2, 2: 4}  # HALF, FLOAT
        dtypes = {1: np.float16, 2: np.float32}
        planes = {name: np.zeros((h, w), np.float32) for name, _ in chans}
        for off in offsets:
            f.seek(off)
            y, size = struct.unpack('<ii', f.read(8))
            raw = zlib.decompress(f.read(size))
            # Undo the delta predictor: t[0] = raw[0], t[i] = t[i-1] + raw[i] - 128 (mod 256).
            b = np.frombuffer(raw, np.uint8).astype(np.int64)
            b[1:] -= 128
            t = (np.cumsum(b) & 255).astype(np.uint8)
            half = (len(t) + 1) // 2
            data = np.empty(len(t), np.uint8)
            data[0::2] = t[:half]
            data[1::2] = t[half:]
            n = min(lines, y1 + 1 - y)
            pos = 0
            for row in range(n):
                for name, ptype in chans:
                    nb = w * sizes[ptype]
                    planes[name][y - y0 + row] = np.frombuffer(data[pos:pos + nb].tobytes(), dtypes[ptype])
                    pos += nb
        return np.stack([planes['R'], planes['G'], planes['B']], axis=-1)
