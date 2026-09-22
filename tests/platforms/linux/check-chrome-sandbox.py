#!/usr/bin/env python3
"""Run in the KDE runner as ubuntu; optional argument is the Plasma PID.

Uses a disposable profile, not the user's open tabs or browsing data.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

env = os.environ.copy()
if len(sys.argv) > 1:
    env.update(item.split('=', 1) for item in
               Path(f'/proc/{int(sys.argv[1])}/environ').read_text().split('\0')
               if '=' in item)
wrapper = Path('/opt/google/chrome/google-chrome').read_text()
assert '--no-sandbox' not in wrapper, 'Chrome wrapper still disables the sandbox'
with tempfile.TemporaryDirectory(prefix='wolf-chrome-sandbox-check-') as profile:
    result = subprocess.run([
        '/usr/bin/google-chrome', '--headless', '--disable-gpu', '--no-first-run',
        '--allow-chrome-scheme-url', '--user-data-dir=' + profile,
        '--dump-dom', 'chrome://sandbox',
    ], env=env, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise SystemExit(result.stderr[-4000:])
    report = re.sub(r'\s+', ' ', re.sub(r'<[^>]+>', ' ', result.stdout))
    for expected in ('Layer 1 Sandbox Namespace', 'PID namespaces Yes',
                     'Network namespaces Yes', 'Seccomp-BPF sandbox Yes',
                     'You are adequately sandboxed.'):
        assert expected in report, f'Missing sandbox assertion: {expected}'
        print('PASS', expected)
