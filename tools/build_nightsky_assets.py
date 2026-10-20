# Copyright 2026 Kim2091 (Sparkles)
# SPDX-License-Identifier: Apache-2.0

"""Build the night sky's data assets (assets/nightsky/) from public sources.

    python tools/build_nightsky_assets.py [--src build/nightsky_src] [--out assets/nightsky]

A game moon's own surface (NightSkyAssets::gameMoons), from any equirectangular colour map
(sRGB, longitude 0 at the centre, north up) and an optional grey height map:

    python tools/build_nightsky_assets.py --custom-moon albedo.png [--custom-moon-height height.png
        --height-range-km -8 8] --custom-out moon_game0.bin

Only the map's relative brightness and colour matter: NUMOS rescales it so its near side averages
the moon's measured brightness and colour (moon<N>.color tints that).

Sources (downloaded into --src when missing; see assets/nightsky/CREDITS.md):
  - Hipparcos main catalogue (ESA 1997, CDS I/239): positions, V, B-V.
  - NASA SVS "Deep Star Maps 2020" (Ernie Wright, NASA/GSFC SVS; Gaia DR2: ESA/Gaia/DPAC):
    milkyway_2020_4k.exr (Gaia stars fainter than Tycho) and hiptyc_2020_4k.exr (Hipparcos + Tycho).
  - NASA SVS "CGI Moon Kit" (LRO LROC WAC colour mosaic, LOLA elevation): lroc_color_poles_2k.tif,
    ldem_4.tif.

Units. Every radiance and flux is a fraction of the sun's illuminance at the top of the atmosphere
(per sr for radiances), times kAssetScale so it fits half floats; the shaders multiply by
sunIlluminance (which carries the pre-exposure) and 1/kAssetScale. V magnitudes convert with the
sun's V = -26.74. Colours are relative to the sun's: a solar-type star is (1, 1, 1) x its luminance.

Calibration of the star maps (measured on the 4k maps, see docs/night-sky.md):
  - they are linear and exactly related: starmap = 0.5 milkyway + hiptyc;
  - isolated Hipparcos stars of V 4.5-5.5 give a hiptyc window flux of C = 2.6e-4 per unit
    10^(-0.4 V) (map value x sr);
  - so the Milky Way background (Gaia, fainter than V ~11.5) is 0.5 milkyway / C in V=0 units per sr.
    Check: the north galactic pole totals ~48 S10 (23.6 V mag/arcsec^2) with the catalogue's
    bright stars and the Tycho band, against the measured ~23.5.

Outputs (little-endian):
  stars.bin           header {magic 'NSTR', version 1, starCount, cellsPerFace, recordCount}, then
                      (6 cellsPerFace^2 + 1) uint32 cell offsets into the records, then records of
                      8 floats: equatorial unit direction xyz, B-V, illuminance rgb (x kAssetScale),
                      a per-star random in [0, 1) (scintillation phase). A star is listed in every
                      cell its disk of kCellMarginDeg reaches, so a lookup reads one cell.
  milkyway.bin        header {magic 'NMWY', version 1, width, height}, then width x height RGB9E5
                      texels (uint32): radiance (x kAssetScale) of every star fainter than the point
                      catalogue, equirectangular in equatorial coordinates (u = 0.5 - RA/360,
                      v = (90 - Dec)/180).
  starlight_low.bin   header {magic 'NSLO', version 1, width, height}, then RGBA16F texels: the
                      whole starlight (points + background) blurred to ~3 deg, same mapping. The sky
                      bake carries this (so clouds and ground get starlight); the sky adds the rest.
  moon.bin            header {magic 'NMON', version 1, width, height}, then RGBA8 texels: linear
                      albedo relative to the mean (x 1/kMoonAlbedoRange) and height (km + 10)/20,
                      equirectangular in selenographic coordinates, longitude 0 at the centre,
                      east (Mare Crisium) toward +u. Any 2:1 size; NUMOS uses a power-of-two band.
"""
import argparse
import math
import os
import struct
import sys
import urllib.request

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from exr_zip import read_exr  # noqa: E402

kAssetScale = 1e10
kSunV = -26.74
kPointLimitV = 7.5          # brighter stars are points; fainter ones are in the background texture
kCellsPerFace = 48          # cube-map cells per face edge (~1.9 deg)
kCellMarginDeg = 0.35       # largest star footprint a lookup must find in its own cell
kMapC = 2.6e-4              # hiptyc window flux per 10^(-0.4 V), measured (see the docstring)
kMilkyWaySize = (2048, 1024)
kLowSize = (128, 64)
kMoonSize = (1024, 512)
kMoonAlbedoRange = 3.5

SOURCES = {
    'hip_main.dat': 'https://cdsarc.cds.unistra.fr/ftp/I/239/hip_main.dat',
    'milkyway_2020_4k.exr': 'https://svs.gsfc.nasa.gov/vis/a000000/a004800/a004851/milkyway_2020_4k.exr',
    'hiptyc_2020_4k.exr': 'https://svs.gsfc.nasa.gov/vis/a000000/a004800/a004851/hiptyc_2020_4k.exr',
    'lroc_color_poles_2k.tif': 'https://svs.gsfc.nasa.gov/vis/a000000/a004700/a004720/lroc_color_poles_2k.tif',
    'ldem_4.tif': 'https://svs.gsfc.nasa.gov/vis/a000000/a004700/a004720/ldem_4.tif',
}


# ---------------------------------------------------------------------------------------------
# Colour: blackbody -> CIE XYZ (Wyman, Sloan & Shirley 2013 multi-lobe fit) -> linear sRGB,
# relative to the sun (5772 K), normalised to unit luminance.
# ---------------------------------------------------------------------------------------------
def _g(x, mu, s1, s2):
    s = np.where(x < mu, s1, s2)
    return np.exp(-0.5 * ((x - mu) / s) ** 2)


def cie_xyz(wl):
    x = 1.056 * _g(wl, 599.8, 37.9, 31.0) + 0.362 * _g(wl, 442.0, 16.0, 26.7) - 0.065 * _g(wl, 501.1, 20.4, 26.2)
    y = 0.821 * _g(wl, 568.8, 46.9, 40.5) + 0.286 * _g(wl, 530.9, 16.3, 31.1)
    z = 1.217 * _g(wl, 437.0, 11.8, 36.0) + 0.681 * _g(wl, 459.0, 26.0, 13.8)
    return np.stack([x, y, z], -1)


XYZ_TO_SRGB = np.array([[3.2406, -1.5372, -0.4986], [-0.9689, 1.8758, 0.0415], [0.0557, -0.2040, 1.0570]])
_WL = np.arange(380.0, 781.0, 2.0)
_CMF = cie_xyz(_WL)


def spectrum_rgb(spd):
    """Linear sRGB of a spectral power distribution sampled at _WL."""
    return XYZ_TO_SRGB @ (spd[:, None] * _CMF).sum(0)


def blackbody(t):
    wl = _WL * 1e-9
    return 1.0 / (wl ** 5 * (np.exp(1.4388e-2 / (wl * t)) - 1.0))


SUN_RGB = spectrum_rgb(blackbody(5772.0))


def sun_relative(rgb):
    c = np.maximum(rgb / SUN_RGB, 0.0)
    return c / (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2])


def bv_to_teff(bv):
    # Ballesteros (2012).
    return 4600.0 * (1.0 / (0.92 * bv + 1.7) + 1.0 / (0.92 * bv + 0.62))


# ---------------------------------------------------------------------------------------------
# Cube-map cells: must match starCellOf() in shaders/numos/lib/night_sky.slangh.
# ---------------------------------------------------------------------------------------------
def cell_of(d):
    ax = np.abs(d)
    face = np.argmax(ax, axis=-1)
    m = np.take_along_axis(ax, face[..., None], -1)[..., 0]
    x, y, z = d[..., 0], d[..., 1], d[..., 2]
    u = np.select([face == 0, face == 1, face == 2], [y, x, x], 0.0) / m
    v = np.select([face == 0, face == 1, face == 2], [z, z, y], 0.0) / m
    sign = np.take_along_axis(d, face[..., None], -1)[..., 0] < 0
    f = face * 2 + sign
    iu = np.clip(((u * 0.5 + 0.5) * kCellsPerFace).astype(np.int64), 0, kCellsPerFace - 1)
    iv = np.clip(((v * 0.5 + 0.5) * kCellsPerFace).astype(np.int64), 0, kCellsPerFace - 1)
    return (f * kCellsPerFace + iv) * kCellsPerFace + iu


def equatorial(ra_deg, de_deg):
    ra, de = np.radians(ra_deg), np.radians(de_deg)
    return np.stack([np.cos(de) * np.cos(ra), np.cos(de) * np.sin(ra), np.sin(de)], -1)


def map_dirs(w, h):
    u = (np.arange(w) + 0.5) / w
    v = (np.arange(h) + 0.5) / h
    ra = np.degrees(((0.5 - u) % 1.0) * 2.0 * np.pi)
    de = 90.0 - v * 180.0
    return equatorial(ra[None, :], de[:, None])


def solid_angle(w, h):
    lat = (0.5 - (np.arange(h) + 0.5) / h) * np.pi
    return (np.cos(lat) * (np.pi / h) * (2.0 * np.pi / w))[:, None]


# ---------------------------------------------------------------------------------------------
def fetch(src):
    os.makedirs(src, exist_ok=True)
    for name, url in SOURCES.items():
        path = os.path.join(src, name)
        if not os.path.exists(path):
            print('downloading', url)
            urllib.request.urlretrieve(url, path)


def load_hipparcos(path):
    rows = []
    with open(path, 'r', encoding='latin-1') as f:
        for line in f:
            try:
                v, ra, de = float(line[41:46]), float(line[51:63]), float(line[64:76])
            except ValueError:
                continue
            bv = line[245:251].strip()
            rows.append((v, ra, de, float(bv) if bv else 0.65))
    return np.array(rows)


def build_stars(cat):
    pts = cat[cat[:, 0] <= kPointLimitV]
    pts = pts[np.argsort(pts[:, 0])]
    dirs = equatorial(pts[:, 1], pts[:, 2])
    flux = 10.0 ** (-0.4 * (pts[:, 0] - kSunV)) * kAssetScale
    rgb = np.array([sun_relative(spectrum_rgb(blackbody(bv_to_teff(bv)))) for bv in pts[:, 3]]) * flux[:, None]
    rnd = np.random.default_rng(7).random(len(pts))

    # Cells each star's margin disk reaches: the centre plus two rings of samples.
    cells = [set() for _ in range(len(pts))]
    tangent = np.cross(dirs, np.where(np.abs(dirs[:, 2:3]) < 0.9, [[0, 0, 1.0]], [[1.0, 0, 0]]))
    tangent /= np.linalg.norm(tangent, axis=1, keepdims=True)
    bitangent = np.cross(dirs, tangent)
    samples = [dirs]
    for frac in (0.5, 1.0):
        r = math.radians(kCellMarginDeg) * frac
        for k in range(12):
            a = 2.0 * math.pi * k / 12
            samples.append(dirs * math.cos(r) + (tangent * math.cos(a) + bitangent * math.sin(a)) * math.sin(r))
    for s in samples:
        for i, c in enumerate(cell_of(s)):
            cells[i].add(int(c))
    ncell = 6 * kCellsPerFace * kCellsPerFace
    lists = [[] for _ in range(ncell)]
    for i, cs in enumerate(cells):
        for c in cs:
            lists[c].append(i)
    offsets = np.zeros(ncell + 1, np.uint32)
    order = []
    for c in range(ncell):
        offsets[c] = len(order)
        order.extend(lists[c])
    offsets[ncell] = len(order)
    rec = np.zeros((len(order), 8), np.float32)
    idx = np.array(order)
    rec[:, 0:3] = dirs[idx]
    rec[:, 3] = pts[idx, 3]
    rec[:, 4:7] = rgb[idx]
    rec[:, 7] = rnd[idx]
    print(f'stars: {len(pts)} brighter than V {kPointLimitV}, {len(order)} records in {ncell} cells '
          f'(max {max(len(l) for l in lists)} per cell)')
    header = struct.pack('<4sIIII', b'NSTR', 1, len(pts), kCellsPerFace, len(order))
    return header + offsets.tobytes() + rec.tobytes(), pts, rgb


def float_to_rgb9e5(rgb):
    """Pack non-negative linear RGB into the shared-exponent E5B9G9R9 format."""
    rgb = np.clip(rgb, 0.0, 65408.0).astype(np.float64)
    maxc = rgb.max(-1)
    exp_shared = np.maximum(-16, np.floor(np.log2(np.maximum(maxc, 1e-30)))) + 1 + 15
    exp_shared = np.clip(exp_shared, 0, 31)
    denom = 2.0 ** (exp_shared - 15 - 9)
    maxm = np.floor(maxc / denom + 0.5)
    bump = maxm == 512
    denom = np.where(bump, denom * 2, denom)
    exp_shared = np.where(bump, exp_shared + 1, exp_shared)
    m = np.floor(rgb / denom[..., None] + 0.5).astype(np.uint32)
    m = np.minimum(m, 511)
    return (m[..., 0] | (m[..., 1] << 9) | (m[..., 2] << 18) | (exp_shared.astype(np.uint32) << 27)).astype(np.uint32)


def downsample(img, w, h):
    H, W = img.shape[:2]
    fy, fx = H // h, W // w
    return img[:h * fy, :w * fx].reshape(h, fy, w, fx, -1).mean((1, 3))


def blur_sphere(img, sigma_deg):
    """Gaussian blur of an equirectangular image on the sphere (separable, latitude-aware)."""
    h, w, _ = img.shape
    out = np.zeros_like(img)
    lat = (0.5 - (np.arange(h) + 0.5) / h) * np.pi
    sig = math.radians(sigma_deg)
    ky = np.arange(-h, h + 1) * (math.pi / h)
    # rows: vertical blur
    tmp = np.zeros_like(img)
    kv = np.exp(-0.5 * (ky / sig) ** 2)
    for y in range(h):
        ys = np.arange(h)
        wts = kv[ys - y + h]
        tmp[y] = (img * wts[:, None, None]).sum(0) / wts.sum()
    for y in range(h):
        s = sig / max(math.cos(lat[y]), 1e-3)
        kx = np.arange(w) * (2.0 * math.pi / w)
        kx = np.minimum(kx, 2.0 * math.pi - kx)
        wts = np.exp(-0.5 * (kx / s) ** 2)
        for c in range(img.shape[2]):
            out[y, :, c] = np.real(np.fft.ifft(np.fft.fft(tmp[y, :, c]) * np.fft.fft(wts))) / wts.sum()
    return out


def build_milky_way(src, pts):
    mw = read_exr(os.path.join(src, 'milkyway_2020_4k.exr')).astype(np.float64)
    ht = read_exr(os.path.join(src, 'hiptyc_2020_4k.exr')).astype(np.float64)
    H, W, _ = mw.shape
    # The Tycho band (V 7.5 .. 11.5): hiptyc with the point stars masked out and filled from their
    # neighbourhood, blurred well below the texture's resolution.
    mask = np.zeros((H, W), bool)
    xs = (((0.5 - pts[:, 1] / 360.0) % 1.0) * W).astype(int) % W
    ys = np.clip(((90.0 - pts[:, 2]) / 180.0 * H).astype(int), 0, H - 1)
    for dy in range(-2, 3):
        for dx in range(-2, 3):
            mask[np.clip(ys + dy, 0, H - 1), (xs + dx) % W] = True
    faint = ht.copy()
    faint[mask] = np.nan
    faint = downsample(np.nan_to_num(faint, nan=0.0), kMilkyWaySize[0], kMilkyWaySize[1]) \
          / np.maximum(downsample((~mask[..., None]).astype(np.float64), kMilkyWaySize[0], kMilkyWaySize[1]), 0.25)
    background = downsample(0.5 * mw, kMilkyWaySize[0], kMilkyWaySize[1]) + faint
    # The maps' colours are white at D65-like blackbodies; ours are white at the sun.
    background = background / (SUN_RGB / (0.2126 * SUN_RGB[0] + 0.7152 * SUN_RGB[1] + 0.0722 * SUN_RGB[2]))[None, None, :] \
                 * 1.0
    # map units per sr -> V=0 per sr -> fraction of the sun's illuminance per sr.
    radiance = background / kMapC * 10.0 ** (-0.4 * (0.0 - kSunV)) * kAssetScale
    print('milky way: radiance x scale  median %.3g  max %.3g' % (np.median(radiance), radiance.max()))
    w, h = kMilkyWaySize
    payload = float_to_rgb9e5(radiance.astype(np.float32))
    return struct.pack('<4sIII', b'NMWY', 1, w, h) + payload.tobytes(), radiance


def build_low(radiance, pts, rgb):
    w, h = kMilkyWaySize
    total = radiance.copy()
    xs = (((0.5 - pts[:, 1] / 360.0) % 1.0) * w).astype(int) % w
    ys = np.clip(((90.0 - pts[:, 2]) / 180.0 * h).astype(int), 0, h - 1)
    om = solid_angle(w, h)[:, 0]
    np.add.at(total, (ys, xs), rgb / om[ys][:, None])
    low = downsample(total, kLowSize[0], kLowSize[1])
    low = blur_sphere(low, 3.0)
    print('starlight low: median %.3g  max %.3g' % (np.median(low), low.max()))
    lw, lh = kLowSize
    rgba = np.concatenate([low, np.ones((lh, lw, 1))], -1).astype(np.float16)
    return struct.pack('<4sIII', b'NSLO', 1, lw, lh) + rgba.tobytes()


def build_moon(src):
    from PIL import Image
    Image.MAX_IMAGE_PIXELS = None
    col = np.asarray(Image.open(os.path.join(src, 'lroc_color_poles_2k.tif')).convert('RGB')).astype(np.float64) / 255.0
    lin = np.where(col <= 0.04045, col / 12.92, ((col + 0.055) / 1.055) ** 2.4)
    w, h = kMoonSize
    lin = downsample(lin, w, h)
    lum = lin @ [0.2126, 0.7152, 0.0722]
    lat = (0.5 - (np.arange(h) + 0.5) / h) * np.pi
    mean = (lum * np.cos(lat)[:, None]).sum() / (np.cos(lat).sum() * w)
    albedo = lin / mean
    dem = np.asarray(Image.open(os.path.join(src, 'ldem_4.tif')), np.float64)
    dem = np.asarray(Image.fromarray(dem.astype(np.float32)).resize((w, h), Image.BILINEAR), np.float64)
    rgba = np.zeros((h, w, 4), np.uint8)
    rgba[..., :3] = np.clip(albedo / kMoonAlbedoRange * 255.0 + 0.5, 0, 255).astype(np.uint8)
    rgba[..., 3] = np.clip((dem + 10.0) / 20.0 * 255.0 + 0.5, 0, 255).astype(np.uint8)
    print('moon: albedo range %.2f..%.2f of mean, height %.1f..%.1f km' % (albedo.min(), albedo.max(), dem.min(), dem.max()))
    return struct.pack('<4sIII', b'NMON', 1, w, h) + rgba.tobytes()


def build_custom_moon(albedo_path, height_path, height_range_km):
    """An NMON file from a game's own maps (see the module docstring)."""
    from PIL import Image
    Image.MAX_IMAGE_PIXELS = None
    col = np.asarray(Image.open(albedo_path).convert('RGB')).astype(np.float64) / 255.0
    lin = np.where(col <= 0.04045, col / 12.92, ((col + 0.055) / 1.055) ** 2.4)
    h, w = lin.shape[:2]
    lum = lin @ [0.2126, 0.7152, 0.0722]
    lat = (0.5 - (np.arange(h) + 0.5) / h) * np.pi
    mean = max((lum * np.cos(lat)[:, None]).sum() / (np.cos(lat).sum() * w), 1e-6)
    rgba = np.zeros((h, w, 4), np.uint8)
    rgba[..., :3] = np.clip(lin / mean / kMoonAlbedoRange * 255.0 + 0.5, 0, 255).astype(np.uint8)
    rgba[..., 3] = 128  # flat: 0 km
    if height_path:
        lo, hi = height_range_km
        g = np.asarray(Image.open(height_path).convert('F').resize((w, h), Image.BILINEAR), np.float64)
        g = g / (65535.0 if g.max() > 255.0 else 255.0)
        km = lo + g * (hi - lo)
        rgba[..., 3] = np.clip((km + 10.0) / 20.0 * 255.0 + 0.5, 0, 255).astype(np.uint8)
    return struct.pack('<4sIII', b'NMON', 1, w, h) + rgba.tobytes()


def main():
    ap = argparse.ArgumentParser()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument('--src', default=os.path.join(root, 'build', 'nightsky_src'))
    ap.add_argument('--out', default=os.path.join(root, 'assets', 'nightsky'))
    ap.add_argument('--custom-moon', help='colour map of a game moon (see the module docstring)')
    ap.add_argument('--custom-moon-height', help='its grey height map')
    ap.add_argument('--height-range-km', type=float, nargs=2, default=(-8.0, 8.0),
                    help='height of black and white in the height map (within -10..10 km)')
    ap.add_argument('--custom-out', default='moon_game0.bin')
    a = ap.parse_args()
    if a.custom_moon:
        data = build_custom_moon(a.custom_moon, a.custom_moon_height, a.height_range_km)
        with open(a.custom_out, 'wb') as f:
            f.write(data)
        print('wrote', a.custom_out, len(data), 'bytes')
        return
    fetch(a.src)
    os.makedirs(a.out, exist_ok=True)
    cat = load_hipparcos(os.path.join(a.src, 'hip_main.dat'))
    stars, pts, rgb = build_stars(cat)
    mw, radiance = build_milky_way(a.src, pts)
    low = build_low(radiance, pts, rgb)
    moon = build_moon(a.src)
    for name, data in (('stars.bin', stars), ('milkyway.bin', mw), ('starlight_low.bin', low), ('moon.bin', moon)):
        with open(os.path.join(a.out, name), 'wb') as f:
            f.write(data)
        print('wrote', name, len(data), 'bytes')


if __name__ == '__main__':
    main()
