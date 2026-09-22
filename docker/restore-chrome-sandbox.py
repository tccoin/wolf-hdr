#!/usr/bin/env python3
"""Undo the Selkies appliance's unconditional Chrome sandbox bypass at build time."""
from pathlib import Path

wrapper = Path('/opt/google/chrome/google-chrome')
text = wrapper.read_text()
unsafe = 'exec -a "$0" "$HERE/chrome" --no-sandbox --password-store=basic "$@"'
safe = 'exec -a "$0" "$HERE/chrome" --password-store=basic "$@"'
if unsafe in text:
    wrapper.write_text(text.replace(unsafe, safe))
elif safe not in text:
    raise SystemExit('Unknown Chrome wrapper: inspect before changing sandbox flags')
assert '--no-sandbox' not in wrapper.read_text()
