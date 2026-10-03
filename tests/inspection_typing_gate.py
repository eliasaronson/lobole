#!/usr/bin/env python3
"""Require static diagnostics on each seeded error, not simply a failing mypy."""
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

# ctest reads 77 as "skipped". mypy is a DEV tool, not a build dependency:
# the nix shell carries it and CI's build-and-test installs it, but a
# stock-Ubuntu container built from the README's list has no reason to, and
# there this gate cannot run at all — it failed every such job with
# "No module named mypy" rather than saying so.
#
# A skip is only honest while the gate really runs somewhere, so the job that
# installs mypy ALSO runs this script directly (ci.yml, "Typing gate"): a
# dropped package is a red build there, not a quiet skip everywhere.
SKIP = 77
if importlib.util.find_spec('mypy') is None:
    print(f'inspection typing: SKIP — no mypy for this interpreter '
          f'({sys.executable}). nix-shell has it; apt: python3-mypy')
    sys.exit(SKIP)
with tempfile.TemporaryDirectory(prefix='inspection-mypy-') as cache:
    env = dict(os.environ, MYPYPATH=str(ROOT / 'tools'))
    base = [sys.executable, '-m', 'mypy', '--follow-imports=silent',
            '--show-error-codes', '--no-error-summary', '--no-pretty',
            '--cache-dir', cache]
    for name in ('inspection_valid', 'inspection_invalid', 'rpc_valid',
                 'rpc_invalid'):
        path = ROOT / 'tests/typing' / f'{name}.py'
        result = subprocess.run(base + [str(path)], env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        errors = {int(n) for n in re.findall(r':(\d+): error:', result.stdout)}
        expected = {i for i, line in enumerate(path.read_text().splitlines(), 1)
                    if '# type-error' in line}
        assert errors == expected, result.stdout
        assert result.returncode == (1 if expected else 0), result.stdout
    generated = subprocess.run(base + [str(ROOT / 'tools/designer_inspection.py'),
                                      str(ROOT / 'tools/designer_rpc.py')],
                               env=env, text=True, capture_output=True)
    assert generated.returncode == 0, generated.stdout + generated.stderr
print('inspection + rpc typing: valid fixtures and every seeded error verified')
