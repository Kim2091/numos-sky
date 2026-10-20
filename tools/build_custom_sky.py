# Copyright 2026 Kim2091 (Sparkles)
# SPDX-License-Identifier: Apache-2.0

"""Build a game's own star field (a procedural galaxy, or a sky from its own images and star lists)
in the night sky's asset formats (stars.bin, milkyway.bin, starlight_low.bin; see
build_nightsky_assets.py). The renderer needs no change: a host passes these files to
Renderer::setNightSkyAssets instead of the ones in assets/nightsky.

    python tools/build_custom_sky.py --out build/custom_sky [--seed 3] [--preview sky.png]

Procedural galaxy (the default). Everything is relative to Earth's sky, which is 1:
  --stars N             stars brighter than V 7.5 (Earth: ~25000), with Earth's magnitude spread
  --brightness K        the diffuse galaxy's light (Earth: a mean of ~170 S10 over the sky)
  --band-width DEG      scale height of the galactic band in latitude (Earth-like: 9)
  --bulge K             brightness of the central bulge relative to the band (Earth-like: 1)
  --clumpiness K        star clouds along the band (0 = smooth)
  --dust K              dark lanes along the band, with reddening (Earth-like: 1; 0 = none)
  --tint R G B          colour of the diffuse light relative to the sun's
  --galaxy-pole RA DEC  where the galactic north pole sits on the celestial sphere (degrees)
  --galaxy-centre RA DEC  where the bulge is (projected onto the band); default: random
  --no-galaxy           no band at all: an even sky of faint stars

From the game's own data (each replaces its procedural part):
  --background IMG      equirectangular image of the diffuse light, mapped like milkyway.bin
                        (u = 0.5 - RA/360, v = (90 - Dec)/180); EXR (linear) or PNG/JPG (sRGB)
  --background-mean-s10 S  its mean brightness over the sky in S10 (default 170, Earth's)
  --stars-csv FILE      stars as lines "ra_deg, dec_deg, v_mag, b_v"; stars fainter than V 7.5
                        join the diffuse light

Units as in build_nightsky_assets.py. 1 S10 (one 10th-magnitude star per square degree) is
6.61e-12 of the sun's illuminance per steradian.
"""
import argparse
import math
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_nightsky_assets as na  # noqa: E402

kS10 = 10.0 ** (-0.4 * (10.0 - na.kSunV)) / math.radians(1.0) ** 2 * na.kAssetScale  # radiance x scale per S10
kEarthMeanS10 = 170.0     # the real Milky Way texture's mean over the sky (fainter than V 7.5)
kEarthStarCount = 25000   # Hipparcos stars brighter than V 7.5
kStarCountSlope = 0.47    # d log10 N(<V) / dV of Earth's bright stars


def sky_dirs(w, h):
    """Equatorial directions of an equirectangular map's texels (the milkyway.bin mapping)."""
    u = (np.arange(w) + 0.5) / w
    v = (np.arange(h) + 0.5) / h
    ra = ((0.5 - u) % 1.0)[None, :] * 2.0 * np.pi
    de = (0.5 - v)[:, None] * np.pi
    return np.stack(np.broadcast_arrays(np.cos(de) * np.cos(ra), np.cos(de) * np.sin(ra), np.sin(de)), -1)


def unit(v):
    return v / np.linalg.norm(v, axis=-1, keepdims=True)


def galactic_basis(rng, pole, centre):
    z = na.equatorial(*pole) if pole is not None else unit(rng.normal(size=3))
    c = na.equatorial(*centre) if centre is not None else unit(rng.normal(size=3))
    x = unit(c - z * (c @ z))
    return np.stack([x, np.cross(z, x), z])  # rows: galactic x (centre), y, z (pole)


def value_noise(dirs, freq, rng):
    """Smooth 3D value noise on the sphere (trilinear with smoothstep, hashed lattice)."""
    table = rng.random(1 << 16)
    p = dirs * freq + 1000.0
    i = np.floor(p).astype(np.int64)
    f = p - i
    f = f * f * (3.0 - 2.0 * f)
    out = 0.0
    for dz in (0, 1):
        for dy in (0, 1):
            for dx in (0, 1):
                h = ((i[..., 0] + dx) * 73856093 ^ (i[..., 1] + dy) * 19349663 ^ (i[..., 2] + dz) * 83492791) & 0xFFFF
                wgt = (f[..., 0] if dx else 1 - f[..., 0]) * (f[..., 1] if dy else 1 - f[..., 1]) \
                    * (f[..., 2] if dz else 1 - f[..., 2])
                out = out + table[h] * wgt
    return out


def fbm(dirs, base, octaves, rng):
    total, amp, norm = 0.0, 1.0, 0.0
    for o in range(octaves):
        total = total + amp * (value_noise(dirs, base * 2.0 ** o, rng) - 0.5)
        norm += amp
        amp *= 0.55
    return total / norm


def procedural_background(a, basis, rng):
    w, h = na.kMilkyWaySize
    dirs = sky_dirs(w, h)
    g = dirs @ basis.T
    lon = np.arctan2(g[..., 1], g[..., 0])
    lat = np.arcsin(np.clip(g[..., 2], -1.0, 1.0))
    floor = 0.01 + 0.006 * np.cos(lat) ** 2
    if a.no_galaxy:
        lum = floor
        colour = np.ones(3)[None, None, :] * np.ones((h, w, 1))
    else:
        width = math.radians(a.band_width) * (1.0 + 0.6 * np.exp(-0.5 * (lon / 0.6) ** 2))
        # Star clouds along the plane, fading out with latitude so the band keeps its edges.
        clumps = np.exp(a.clumpiness * 3.0 * fbm(dirs, 6.0, 6, rng) * np.exp(-np.abs(lat) / (2.0 * width)))
        disk = np.exp(-np.abs(lat) / width) * (0.55 + 0.45 * np.cos(lon / 2.0) ** 2) * clumps
        bulge = a.bulge * 1.6 * np.exp(-0.5 * ((lon / math.radians(14.0)) ** 2 + (lat / math.radians(10.0)) ** 2))
        # Dark lanes: optical depth hugging the plane, broken up by noise, deepest toward the centre.
        lanes = np.clip(0.5 + fbm(dirs, 7.0, 6, rng) * 3.0, 0.0, None)
        tau = a.dust * 2.5 * lanes * np.exp(-np.abs(lat) / (0.45 * width)) * (0.35 + 0.65 * np.exp(-0.5 * (lon / 1.0) ** 2))
        lum = floor + disk + bulge
        # The bulge's old stars are redder; dust reddens what it lets through.
        warm = bulge / np.maximum(lum, 1e-6)
        colour = (np.array([1.0, 1.0, 0.95])[None, None, :] * (1.0 - warm[..., None])
                  + np.array([1.12, 1.0, 0.78])[None, None, :] * warm[..., None])
        colour = colour * np.exp(-tau[..., None] * np.array([0.9, 1.0, 1.15])[None, None, :]) \
               / np.exp(-tau[..., None])
        lum = lum * np.exp(-tau)
    om = na.solid_angle(w, h)
    mean = (lum * om).sum() / (om.sum() * w)
    s10 = lum / mean * kEarthMeanS10 * a.brightness
    print('diffuse light S10: 5/50/95/99.9 percentiles %s (Earth: 20 / 90 / 590 / 2500)'
          % ' / '.join('%.0f' % np.percentile(s10, q) for q in (5, 50, 95, 99.9)))
    return s10[..., None] * colour * np.array(a.tint)[None, None, :] * kS10


def image_background(path, mean_s10):
    w, h = na.kMilkyWaySize
    if path.lower().endswith('.exr'):
        img = na.read_exr(path).astype(np.float64)
    else:
        from PIL import Image
        Image.MAX_IMAGE_PIXELS = None
        c = np.asarray(Image.open(path).convert('RGB')).astype(np.float64) / 255.0
        img = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    from PIL import Image
    img = np.stack([np.asarray(Image.fromarray(img[..., k].astype(np.float32)).resize((w, h), Image.BILINEAR))
                    for k in range(3)], -1).astype(np.float64)
    lum = img @ [0.2126, 0.7152, 0.0722]
    om = na.solid_angle(w, h)
    mean = max((lum * om).sum() / (om.sum() * w), 1e-12)
    return img / mean * mean_s10 * kS10


def procedural_stars(a, basis, rng):
    n = int(a.stars)
    v = 7.5 + np.log10(rng.random(n)) / kStarCountSlope
    while (bad := v < -1.6).any():
        v[bad] = 7.5 + np.log10(rng.random(bad.sum())) / kStarCountSlope
    # Fainter stars crowd toward the band more (they are, on average, farther away).
    concentrated = rng.random(n) < (0 if a.no_galaxy else np.clip(0.1 + 0.05 * v, 0.05, 0.5))
    d = unit(rng.normal(size=(n, 3)))
    k = concentrated.sum()
    lon = rng.normal(0.0, 1.6, k)
    lat = np.clip(rng.laplace(0.0, math.radians(a.band_width) * 1.2, k), -0.5 * math.pi, 0.5 * math.pi)
    g = np.stack([np.cos(lat) * np.cos(lon), np.cos(lat) * np.sin(lon), np.sin(lat)], -1)
    d[concentrated] = g @ basis
    bv = np.where(rng.random(n) < 0.7, rng.normal(0.55, 0.3, n), rng.normal(1.25, 0.25, n))
    ra = np.degrees(np.arctan2(d[:, 1], d[:, 0])) % 360.0
    de = np.degrees(np.arcsin(np.clip(d[:, 2], -1, 1)))
    return np.stack([v, ra, de, np.clip(bv, -0.35, 2.0)], -1)


def csv_stars(path):
    rows = []
    for line in open(path, encoding='utf-8'):
        parts = line.replace(';', ',').split(',')
        try:
            rows.append([float(parts[2]), float(parts[0]), float(parts[1]), float(parts[3]) if len(parts) > 3 else 0.65])
        except (ValueError, IndexError):
            continue  # header or blank
    return np.array(rows)


def add_faint_stars(radiance, cat):
    faint = cat[cat[:, 0] > na.kPointLimitV]
    if len(faint) == 0:
        return
    w, h = na.kMilkyWaySize
    flux = 10.0 ** (-0.4 * (faint[:, 0] - na.kSunV)) * na.kAssetScale
    rgb = np.array([na.sun_relative(na.spectrum_rgb(na.blackbody(na.bv_to_teff(b)))) for b in faint[:, 3]]) * flux[:, None]
    xs = (((0.5 - faint[:, 1] / 360.0) % 1.0) * w).astype(int) % w
    ys = np.clip(((90.0 - faint[:, 2]) / 180.0 * h).astype(int), 0, h - 1)
    np.add.at(radiance, (ys, xs), rgb / na.solid_angle(w, h)[ys])


def preview(path, radiance, cat):
    from PIL import Image
    img = radiance / kS10
    w, h = na.kMilkyWaySize
    pts = cat[cat[:, 0] <= na.kPointLimitV]
    xs = (((0.5 - pts[:, 1] / 360.0) % 1.0) * w).astype(int) % w
    ys = np.clip(((90.0 - pts[:, 2]) / 180.0 * h).astype(int), 0, h - 1)
    star = 10.0 ** (-0.4 * (pts[:, 0] - 10.0)) / (math.degrees(math.pi / h) ** 2)  # S10 if spread over a texel
    np.add.at(img, (ys, xs), star[:, None])
    out = img / (img + 400.0)
    Image.fromarray((np.clip(out, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)).save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default='build/custom_sky')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--stars', type=float, default=kEarthStarCount)
    ap.add_argument('--brightness', type=float, default=1.0)
    ap.add_argument('--band-width', type=float, default=9.0)
    ap.add_argument('--bulge', type=float, default=1.0)
    ap.add_argument('--clumpiness', type=float, default=1.0)
    ap.add_argument('--dust', type=float, default=1.0)
    ap.add_argument('--tint', type=float, nargs=3, default=(1.0, 1.0, 1.0))
    ap.add_argument('--galaxy-pole', type=float, nargs=2)
    ap.add_argument('--galaxy-centre', type=float, nargs=2)
    ap.add_argument('--no-galaxy', action='store_true')
    ap.add_argument('--background')
    ap.add_argument('--background-mean-s10', type=float, default=kEarthMeanS10)
    ap.add_argument('--stars-csv')
    ap.add_argument('--preview', help='also write an equirectangular preview PNG')
    a = ap.parse_args()

    rng = np.random.default_rng(a.seed)
    basis = galactic_basis(rng, a.galaxy_pole, a.galaxy_centre)
    radiance = image_background(a.background, a.background_mean_s10) if a.background \
        else procedural_background(a, basis, rng)
    cat = csv_stars(a.stars_csv) if a.stars_csv else procedural_stars(a, basis, rng)
    add_faint_stars(radiance, cat)

    stars, pts, rgb = na.build_stars(cat)
    w, h = na.kMilkyWaySize
    mw = struct.pack('<4sIII', b'NMWY', 1, w, h) + na.float_to_rgb9e5(radiance.astype(np.float32)).tobytes()
    low = na.build_low(radiance, pts, rgb)
    os.makedirs(a.out, exist_ok=True)
    for name, data in (('stars.bin', stars), ('milkyway.bin', mw), ('starlight_low.bin', low)):
        with open(os.path.join(a.out, name), 'wb') as f:
            f.write(data)
        print('wrote', os.path.join(a.out, name), len(data), 'bytes')
    if a.preview:
        preview(a.preview, radiance, cat)
        print('wrote', a.preview)


if __name__ == '__main__':
    main()
