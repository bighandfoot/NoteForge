"""Runs the FL Studio bridge script against the exact SysEx bytes the plugin sends,
with stand-in 'transport' and 'midi' modules (the real ones only exist inside FL Studio).
Usage: python test_fl_bridge_script.py <device_NoteForgeBridge.py> <fl_bridge_messages.bin>"""
import sys
import types
import importlib.util

calls = []
playing = {"v": False}

transport = types.ModuleType("transport")
transport.setSongPos = lambda pos, mode=-1: calls.append(("setSongPos", pos, mode))
transport.isPlaying = lambda: playing["v"]
def _start():
    calls.append(("start",)); playing["v"] = True
def _stop():
    calls.append(("stop",)); playing["v"] = False
transport.start, transport.stop = _start, _stop
midi = types.ModuleType("midi")
midi.SONGLENGTH_MS = 0
sys.modules["transport"], sys.modules["midi"] = transport, midi

spec = importlib.util.spec_from_file_location("bridge", sys.argv[1])
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class Event:
    def __init__(self, data):
        self.sysex = data
        self.handled = False


raw = open(sys.argv[2], "rb").read()
i = 0
events = []
while i < len(raw):
    n = raw[i]
    events.append(bytes(raw[i + 1:i + 1 + n]))
    i += 1 + n

for data in events:
    e = Event(data)
    bridge.OnSysEx(e)
    assert e.handled, "message not handled"

# an unrelated SysEx (e.g. from a hardware controller) must be ignored
other = Event(bytes([0xF0, 0x47, 0x7F, 0x15, 0x01, 0xF7]))
bridge.OnSysEx(other)
assert not other.handled, "foreign SysEx was handled"

expected = [("setSongPos", 83457, 0), ("setSongPos", 3600250, 0), ("start",), ("stop",), ("setSongPos", 0, 0)]
assert calls == expected, "got %r" % (calls,)
print("FL bridge script OK:", calls)
