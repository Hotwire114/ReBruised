"""Load the scripted xinput1_4.dll and print what slot 0 reports over a few seconds.
    BNB_PAD_SCRIPT="1:1000:1;2.5:10:1" python -I tools/xinput_script/test_dll.py build/program/bin/xinput1_4.dll
"""
import ctypes
import sys
import time


class Pad(ctypes.Structure):
    _fields_ = [("wButtons", ctypes.c_ushort), ("lt", ctypes.c_ubyte), ("rt", ctypes.c_ubyte),
                ("lx", ctypes.c_short), ("ly", ctypes.c_short), ("rx", ctypes.c_short), ("ry", ctypes.c_short)]


class State(ctypes.Structure):
    _fields_ = [("packet", ctypes.c_ulong), ("pad", Pad)]


dll = ctypes.WinDLL(sys.argv[1])
t0 = time.time()
last = None
while time.time() - t0 < 4.0:
    s = State()
    r = dll.XInputGetState(0, ctypes.byref(s))
    v = (r, s.pad.wButtons)
    if v != last:
        print(f"t={time.time() - t0:5.2f} ret={r} buttons={s.pad.wButtons:#06x} packet={s.packet}")
        last = v
    time.sleep(0.02)
