# Copyright 2026 Kim2091 (Sparkles)
# SPDX-License-Identifier: Apache-2.0

"""Rebuild the shaders and every file generated from them, in order.

Usage: python tools/regen.py [--check]

1. tools/build_shaders.py     -> build/spirv/*.spv
2. tools/gen_layout_checks.py -> include/numos/detail/layout_checks.h
3. tools/gen_pass_table.py    -> src/generated/passes.inc
4. the viewer's shaders and pass table (build/viewer_spirv, viewer/generated/viewer_passes.inc):
   a stale viewer table binds the composite's textures to the wrong slots, silently.

Run after any shader change. With --check, fails if a generated file is out of date (for CI).
"""
import subprocess
import sys
import pathlib

TOOLS = pathlib.Path(__file__).resolve().parent
check = ['--check'] if '--check' in sys.argv else []

ROOT = TOOLS.parent
for step in (['build_shaders.py'], ['gen_layout_checks.py', *check], ['gen_pass_table.py', *check],
             ['build_shaders.py', '--src', str(ROOT / 'viewer' / 'shaders'), '--out', str(ROOT / 'build' / 'viewer_spirv')],
             ['gen_pass_table.py', '--spirv', str(ROOT / 'build' / 'viewer_spirv'),
              '--out', str(ROOT / 'viewer' / 'generated' / 'viewer_passes.inc'), *check]):
    r = subprocess.run([sys.executable, str(TOOLS / step[0]), *step[1:]])
    if r.returncode != 0:
        sys.exit(f'{step[0]} failed')
print('regen complete')
