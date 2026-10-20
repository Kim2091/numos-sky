# Copyright 2026 Kim2091 (Sparkles)
# SPDX-License-Identifier: Apache-2.0

"""Compile every NUMOS compute pass to SPIR-V, standalone.

Usage:
  python tools/build_shaders.py [--slangc PATH] [--out DIR] [--jobs N] [--only SUBSTRING]

slangc comes from --slangc, then the NUMOS_SLANGC environment variable, then the
repository's git config value numos.slangc (git config numos.slangc <path>), then
slangc on PATH. A Remix tree has a packaged copy at external/slang/slangc.exe.
Flags match Remix's compile_shaders.py, so the SPIR-V is what Remix itself would build.

Variants use Remix's comment syntax at the top of a .slang file:
  //!variant <name>        one output per variant
  //!> DEFINE DEFINE=1     defines for the variant above (may repeat)
  //!end-variants
A file without variants builds once, named after the file (minus .slang).
"""
import argparse
import concurrent.futures
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SHADERS = ROOT / 'shaders'
FLAGS = ['-entry', 'main', '-target', 'spirv', '-zero-initialize', '-emit-spirv-directly',
         '-D__SLANG__', '-matrix-layout-column-major', '-Wno-30081', '-fvk-use-scalar-layout']


def default_slangc():
    """NUMOS_SLANGC, then git config numos.slangc, then slangc on PATH. Empty when none is set."""
    if os.environ.get('NUMOS_SLANGC'):
        return os.environ['NUMOS_SLANGC']
    r = subprocess.run(['git', '-C', str(ROOT), 'config', '--get', 'numos.slangc'], capture_output=True, text=True)
    if r.returncode == 0 and r.stdout.strip():
        return r.stdout.strip()
    return shutil.which('slangc') or ''


def variants(path):
    """[(name, [defines])] for one .slang file."""
    out = []
    for line in path.read_text(encoding='utf-8').splitlines():
        s = line.strip()
        if s.startswith('//!variant'):
            out.append((s.split(None, 1)[1].strip(), []))
        elif s.startswith('//!>') and out:
            out[-1][1].extend(s[4:].split())
        elif s.startswith('//!end-variants'):
            break
    return out or [(path.name[:-len('.slang')], [])]


def compile_one(slangc, src, name, defines, outdir, includes):
    spv = outdir / (name + '.spv')
    cmd = [str(slangc), str(src), '-I', str(SHADERS), *[a for i in includes for a in ('-I', str(i))], *FLAGS, *('-D' + d for d in defines), '-o', str(spv)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return name, r.returncode, (r.stdout + r.stderr).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--slangc', default=None)
    ap.add_argument('--out', default=str(ROOT / 'build' / 'spirv'))
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--only', default='')
    ap.add_argument('--src', default=str(SHADERS), help='directory searched for *.comp.slang')
    a = ap.parse_args()

    found = a.slangc or default_slangc()
    if not found:
        sys.exit('slangc not found: pass --slangc, set NUMOS_SLANGC or git config numos.slangc, or put slangc on PATH')
    slangc = pathlib.Path(found)
    if not slangc.is_file():
        sys.exit(f'slangc not found: {slangc}')
    outdir = pathlib.Path(a.out)
    outdir.mkdir(parents=True, exist_ok=True)

    jobs = []
    srcdir = pathlib.Path(a.src)
    for src in sorted(srcdir.rglob("*.comp.slang")):
        for name, defines in variants(src):
            if a.only in name:
                jobs.append((src, name, defines))

    failed = 0
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        for name, rc, log in pool.map(lambda j: compile_one(slangc, j[0], j[1], j[2], outdir, [srcdir]), jobs):
            if rc != 0:
                failed += 1
                print(f'FAILED {name}\n{log[-2500:]}\n')
            else:
                print(f'ok     {name}' + (f'  ({len(log.splitlines())} lines of warnings)' if log else ''))
    print(f'{len(jobs) - failed}/{len(jobs)} variants built into {outdir}')
    # The pass table is generated from every .spv in the directory, so a variant whose source is gone
    # must not linger there (a full build only; --only leaves the rest alone).
    if not a.only:
        built = {name + '.spv' for _, name, _ in jobs}
        for spv in outdir.glob('*.spv'):
            if spv.name not in built:
                spv.unlink()
                print(f'removed stale {spv.name}')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
