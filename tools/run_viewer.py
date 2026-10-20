# Copyright 2026 Kim2091 (Sparkles)
# SPDX-License-Identifier: Apache-2.0

"""Run the headless NUMOS viewer and convert its output to PNG.

Usage: python tools/run_viewer.py out.png [viewer options...]
Example: python tools/run_viewer.py build/shots/sunset.png --sun-elevation 4 --conf path/to/rtx.conf

Viewer options: --conf FILE, --set key=value (repeatable), --sun-elevation DEG, --sun-azimuth DEG,
--yaw DEG, --pitch DEG, --eye-height M, --width W, --height H, --frames N, --exposure E.
--denoise (with --accumulate): also run Intel Open Image Denoise on the accumulated HDR image;
out.png is then the denoised image and out_raw.png the plain accumulation (tools/denoise_ref.py).
Needs build/spirv (tools/build_shaders.py), build/viewer_spirv
(tools/build_shaders.py --src viewer/shaders --out build/viewer_spirv) and build/cmake/numos_viewer.exe.
"""
import pathlib
import subprocess
import sys

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent

if len(sys.argv) < 2:
    sys.exit(__doc__)
out = pathlib.Path(sys.argv[1])
out.parent.mkdir(parents=True, exist_ok=True)
ppm = out.with_suffix('.ppm')
args = sys.argv[2:]
denoise = '--denoise' in args
if denoise:
    args = [a for a in args if a != '--denoise']
    pfm = out.with_suffix('.pfm')
    args += ['--hdr-out', str(pfm)]
r = subprocess.run([str(ROOT / 'build/cmake/numos_viewer.exe'), '--root', str(ROOT), '--out', str(ppm), *args])
if r.returncode != 0:
    sys.exit(r.returncode)
if denoise:
    raw = out.with_name(out.stem + '_raw' + out.suffix)
    Image.open(ppm).save(raw)
    subprocess.run([sys.executable, str(ROOT / 'tools/denoise_ref.py'), str(pfm), str(out)], check=True,
                   stdout=subprocess.DEVNULL)
    pfm.unlink()
else:
    Image.open(ppm).save(out)
ppm.unlink()
print(f'wrote {out}')
