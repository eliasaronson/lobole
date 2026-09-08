#!/usr/bin/env python3
"""Require static diagnostics on each seeded error, not simply a failing mypy."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='inspection-mypy-') as cache:
    env = dict(os.environ, MYPYPATH=str(ROOT / 'tools'))
    base = [sys.executable, '-m', 'mypy', '--follow-imports=silent',
            '--show-error-codes', '--no-error-summary', '--no-pretty',
            '--cache-dir', cache]
    for name in ('valid', 'invalid'):
        path = ROOT / 'tests/typing' / f'inspection_{name}.py'
        result = subprocess.run(base + [str(path)], env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        errors = {int(n) for n in re.findall(r':(\d+): error:', result.stdout)}
        expected = {i for i, line in enumerate(path.read_text().splitlines(), 1)
                    if '# type-error' in line}
        assert errors == expected, result.stdout
        assert result.returncode == (1 if expected else 0), result.stdout
    generated = subprocess.run(base + [str(ROOT / 'tools/designer_inspection.py')],
                               env=env, text=True, capture_output=True)
    assert generated.returncode == 0, generated.stdout + generated.stderr
print('inspection typing: valid fixture and every seeded error verified')
