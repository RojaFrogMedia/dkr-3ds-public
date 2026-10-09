#!/usr/bin/env python3
"""Map the connected game controller onto the 3DS buttons in the portable
Azahar under tools/azahar, so the game can be played on the PC with it.

    python tools/map-controller.py

Needs `pip install pysdl2 pysdl2-dll`. The buttons are mapped by their
labels: A, B, X, Y as printed, LB/RB = L/R, LT/RT = ZL/ZR, Start = START,
Back = SELECT, left stick = Circle Pad, right stick = C-Stick, D-pad = D-pad.
If the emulator does not react to the controller afterwards, its own
Emulation > Configure > Controls > Auto Map does the same job.
"""
import ctypes
import glob
import os
import re
import sys

import sdl2

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BS = chr(92)

sdl2.SDL_Init(sdl2.SDL_INIT_JOYSTICK | sdl2.SDL_INIT_GAMECONTROLLER)
index = next((i for i in range(sdl2.SDL_NumJoysticks()) if sdl2.SDL_IsGameController(i)), None)
if index is None:
    sys.exit("no game controller is connected")
buf = ctypes.create_string_buffer(40)
sdl2.SDL_JoystickGetGUIDString(sdl2.SDL_JoystickGetDeviceGUID(index), buf, 40)
guid = buf.value.decode()
name = sdl2.SDL_JoystickNameForIndex(index).decode()
mapping = sdl2.SDL_GameControllerMappingForDeviceIndex(index).decode()
print(f"controller: {name} ({guid})")

# SDL's description of the pad: "leftx:a0,a:b0,dpup:h0.1,...".
binds = dict(part.split(":", 1) for part in mapping.split(",")[2:] if ":" in part)
base = f"engine:sdl,guid:{guid},port:0"


def button(sdl_name):
    b = binds.get(sdl_name, "")
    if b.startswith("b"):
        return f"{base},button:{b[1:]}"
    if b.startswith("h"):
        hat, mask = b[1:].split(".")
        direction = {"1": "up", "2": "right", "4": "down", "8": "left"}[mask]
        return f"{base},hat:{hat},direction:{direction}"
    if b.startswith("a") or b.startswith("+a"):
        return f"{base},axis:{b.lstrip('+a')},direction:+,threshold:0.5"
    raise SystemExit(f"controller has no {sdl_name}")


def stick(x_name, y_name):
    return f"{base},axis_x:{binds[x_name][1:]},axis_y:{binds[y_name][1:]}"


values = {
    "button_a": button("a"), "button_b": button("b"), "button_x": button("x"), "button_y": button("y"),
    "button_up": button("dpup"), "button_down": button("dpdown"),
    "button_left": button("dpleft"), "button_right": button("dpright"),
    "button_l": button("leftshoulder"), "button_r": button("rightshoulder"),
    "button_zl": button("lefttrigger"), "button_zr": button("righttrigger"),
    "button_start": button("start"), "button_select": button("back"),
    "circle_pad": stick("leftx", "lefty"), "c_stick": stick("rightx", "righty"),
}

for emulator in glob.glob(os.path.join(ROOT, "tools", "azahar*", "azahar-windows-*")):
    path = os.path.join(emulator, "user", "config", "qt-config.ini")
    if not os.path.exists(path):
        continue
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    prefix = "profiles" + BS + "1" + BS
    for key, value in values.items():
        wanted = {prefix + key: '"' + value + '"', prefix + key + BS + "default": "false"}
        for full_key, new in wanted.items():
            for i, line in enumerate(lines):
                if line.startswith(full_key + "="):
                    lines[i] = full_key + "=" + new
                    break
            else:
                # After the [Controls] header, where Azahar keeps them.
                at = lines.index("[Controls]") + 1 if "[Controls]" in lines else len(lines)
                if "[Controls]" not in lines:
                    lines.append("[Controls]")
                    at = len(lines)
                lines.insert(at, full_key + "=" + new)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print("mapped in", path)
