#!/usr/bin/env python3
"""Read-only: verify the Steam environment's SDL bindings against Linux js0.

Run in the KDE runner as ubuntu; pass the Steam PID. No input is injected.
"""
import array
import ctypes as C
import fcntl
import os
import sys

with open(f"/proc/{int(sys.argv[1])}/environ") as source:
    steam_env = dict(item.split("=", 1) for item in source.read().split("\0") if "=" in item)
os.environ.update(steam_env)
assert os.environ.get("SDL_JOYSTICK_HIDAPI") == "0"
sdl = C.CDLL("libSDL2-2.0.so.0")
assert sdl.SDL_Init(0x2000) == 0

class Value(C.Union):
    _fields_ = [("index", C.c_int), ("hat", C.c_int * 2)]

class Binding(C.Structure):
    _fields_ = [("kind", C.c_int), ("value", Value)]

sdl.SDL_JoystickNameForIndex.restype = C.c_char_p
sdl.SDL_GameControllerOpen.restype = C.c_void_p
sdl.SDL_GameControllerGetBindForButton.argtypes = [C.c_void_p, C.c_int]
sdl.SDL_GameControllerGetBindForButton.restype = Binding
sdl.SDL_GameControllerGetBindForAxis.argtypes = [C.c_void_p, C.c_int]
sdl.SDL_GameControllerGetBindForAxis.restype = Binding
index = next(i for i in range(sdl.SDL_NumJoysticks())
             if b"Wolf DualSense" in sdl.SDL_JoystickNameForIndex(i))
controller = sdl.SDL_GameControllerOpen(index)
assert controller
# This runner exposes one Wolf pad as js0 (motion sensors are js1).
with open("/dev/input/js0", "rb", buffering=0) as joystick:
    buttons = array.array("H", [0] * 512)
    axes = array.array("B", [0] * 64)
    fcntl.ioctl(joystick, 0x84006A34, buttons)  # JSIOCGBTNMAP
    fcntl.ioctl(joystick, 0x80406A32, axes)     # JSIOCGAXMAP
for name, sdl_button, linux_code in [
    ("Cross", 0, 304), ("Circle", 1, 305), ("Square", 2, 308), ("Triangle", 3, 307),
    ("Create", 4, 314), ("PS", 5, 316), ("Options", 6, 315),
    ("L3", 7, 317), ("R3", 8, 318), ("L1", 9, 310), ("R1", 10, 311),
]:
    bind = sdl.SDL_GameControllerGetBindForButton(controller, sdl_button)
    assert bind.kind == 1 and buttons[bind.value.index] == linux_code, name
    print(f"PASS {name}: SDL b{bind.value.index} = Linux {linux_code}")
for name, sdl_axis, linux_axis in [
    ("LX", 0, 0), ("LY", 1, 1), ("RX", 2, 3), ("RY", 3, 4),
    ("L2", 4, 2), ("R2", 5, 5),
]:
    bind = sdl.SDL_GameControllerGetBindForAxis(controller, sdl_axis)
    assert bind.kind == 2 and axes[bind.value.index] == linux_axis, name
    print(f"PASS {name}: SDL a{bind.value.index} = Linux ABS {linux_axis}")
for button, mask in [(11, 1), (12, 4), (13, 8), (14, 2)]:
    bind = sdl.SDL_GameControllerGetBindForButton(controller, button)
    assert bind.kind == 3 and list(bind.value.hat) == [0, mask]
print("PASS all four D-pad directions")
sdl.SDL_Quit()
