# Copyright 2026 Kim2091 (Sparkles)
# SPDX-License-Identifier: Apache-2.0

"""Denoise a viewer HDR dump with Intel Open Image Denoise and tonemap it like the viewer.

Usage: python tools/denoise_ref.py in.pfm out.png [--raw raw.png]
The input comes from the viewer's --hdr-out (linear radiance x exposure, accumulated).
Needs oidnDenoise.exe: set OIDN_DENOISE, or unpack the official release
(github.com/RenderKit/oidn/releases, x64.windows zip) under build/oidn/.

A viewing aid for path-traced references: the denoised image is biased (it is a learned filter),
so fit numbers against the raw accumulation, and keep the raw one beside it when judging detail.
"""
import os
import pathlib
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent


def find_oidn():
    if os.environ.get('OIDN_DENOISE'):
        return os.environ['OIDN_DENOISE']
    hits = sorted((ROOT / 'build/oidn').glob('*/bin/oidnDenoise.exe'))
    if not hits:
        sys.exit('oidnDenoise.exe not found (see tools/denoise_ref.py docstring)')
    return str(hits[-1])


def read_pfm(path):
    with open(path, 'rb') as f:
        assert f.readline().strip() == b'PF'
        w, h = map(int, f.readline().split())
        scale = float(f.readline())
        data = np.frombuffer(f.read(), dtype='<f4' if scale < 0 else '>f4')
    return np.flipud(data.reshape(h, w, 3))


def tonemap(x):
    x = np.maximum(x, 0.0)
    aces = np.clip((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0)
    return Image.fromarray((np.power(aces, 1.0 / 2.2) * 255.0 + 0.5).astype(np.uint8))


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    src, out = pathlib.Path(args[0]), pathlib.Path(args[1])
    den = out.with_suffix('.denoised.pfm')
    subprocess.run([find_oidn(), '--hdr', str(src), '-o', str(den), '-q', 'high'], check=True,
                   stdout=subprocess.DEVNULL)
    tonemap(read_pfm(den)).save(out)
    den.unlink()
    if '--raw' in args:
        tonemap(read_pfm(src)).save(args[args.index('--raw') + 1])
    print(f'wrote {out}')


if __name__ == '__main__':
    main()
