#!/usr/bin/env python3
"""Patch Gamescope's nested virtual EDID for a one-off HDR metadata probe.

Gamescope's current base CTA block has a zero DTD offset despite containing an
HDR static-metadata block. Its normal PatchEdid() refuses to enter that block,
so this diagnostic helper makes the existing block valid and updates its peak.
The production fix belongs in Gamescope's EDID writer, not in this helper.
"""

import argparse
import math
from pathlib import Path


def encode_luminance(nits: int) -> int:
    return min(255, max(1, math.ceil(32 * math.log2(nits / 50))))


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
parser.add_argument('destination', type=Path)
parser.add_argument('--peak', type=int, default=1000)
parser.add_argument('--full-frame', type=int, default=400)
args = parser.parse_args()

edid = bytearray(args.source.read_bytes())
if len(edid) != 256 or edid[126] != 1 or edid[128:132] != bytes([2, 3, 0, 0]):
    parser.error('not the expected two-block nested Gamescope EDID')
if edid[132:134] != bytes([0xE6, 0x06]):
    parser.error('expected CTA HDR static-metadata data block is absent')

edid[130] = 11  # Data blocks end immediately after the seven-byte HDR block.
edid[134:139] = bytes([7, 1, encode_luminance(args.peak),
                       encode_luminance(args.full_frame), 0])
edid[255] = (-sum(edid[128:255])) & 0xFF
assert sum(edid[:128]) % 256 == 0
assert sum(edid[128:]) % 256 == 0
args.destination.write_bytes(edid)
