#!/usr/bin/env python3
"""Check hdr-pq-patches.c RGB10A2 capture; ignore startup/teardown frames.

Usage: check-hdr-pq-capture.py frames.rgb10
The first six patches cover the calibrated 0..550 nit range. 700/1000 nit
patches deliberately exceed the target and may be tone mapped or passed on.
This measures compositor output, not the client's physical screen luminance.
"""
import math
import argparse
from pathlib import Path
import struct

def pq(nits):
    p = (nits / 10000) ** 0.1593017578125
    return ((0.8359375 + 18.8515625 * p) / (1 + 18.6875 * p)) ** 78.84375

def nits(code, maximum):
    p = (code / maximum) ** (1 / 78.84375)
    return 10000 * (max(p - 0.8359375, 0) / (18.8515625 - 18.6875 * p)) ** (1 / 0.1593017578125)

levels = [0, 100, 203, 300, 400, 550]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('capture')
parser.add_argument('--start-frame', type=int, default=0)
parser.add_argument('--end-frame', type=int)
parser.add_argument('--source-depth', type=int, choices=(8,10), default=8)
args = parser.parse_args()
maximum = (1 << args.source_depth) - 1
expected = [nits(math.floor(pq(v) * maximum + 0.5), maximum) for v in levels]
frame_size = 1280 * 720 * 4
passing = 0
with Path(args.capture).open("rb") as capture:
    capture.seek(args.start_frame * frame_size)
    frame_number = args.start_frame
    while args.end_frame is None or frame_number < args.end_frame:
        frame = capture.read(frame_size)
        if len(frame) != frame_size:
            break
        frame_number += 1
        measured = []
        for i in range(len(levels)):
            pixel = struct.unpack_from("<I", frame, (360 * 1280 + 80 + 160 * i) * 4)[0]
            measured.append([nits((pixel >> shift) & 1023, 1023) for shift in (0, 10, 20)])
        if all(abs(channel - value) <= max(0.05, value * 0.015)
               for value, channels in zip(expected, measured) for channel in channels):
            passing += 1
            result = [round(channels[0], 2) for channels in measured]
assert passing >= 3, f"Only {passing} matching frames; expected at least 3"
print(f"PASS {passing} frames: nominal={levels}; measured={result} nit ({args.source_depth}-bit source quantization)")
