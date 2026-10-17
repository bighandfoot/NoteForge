# name=NoteForge Bridge
# url=
"""
NoteForge Bridge - lets the NoteForge plugin move FL Studio's song position and start/stop playback.

Setup (once):
  1. Install loopMIDI and create a port whose name contains "NoteForge" (e.g. "NoteForge").
  2. FL Studio: Options > MIDI settings > Input: select the "NoteForge" port, click Enable,
     and set "Controller type" to "NoteForge Bridge (user)".

Messages (SysEx from the plugin):  F0 7D 4E 46 <cmd> <ms0..ms4> F7
  cmd 1 = set song position (milliseconds), 2 = set position and start playback, 3 = stop
"""
import transport
import midi

_HEADER = (0x7D, 0x4E, 0x46)                       # non-commercial manufacturer ID, 'N', 'F'
_MS = getattr(midi, "SONGLENGTH_MS", 0)            # setSongPos mode: position in milliseconds


def _payload(data):
    """Bytes after the NoteForge header (works whether or not FL includes F0/F7)."""
    for i in range(len(data) - 3):
        if data[i] == _HEADER[0] and data[i + 1] == _HEADER[1] and data[i + 2] == _HEADER[2]:
            return data[i + 3:]
    return None


def _milliseconds(p):
    value = 0
    for i in range(5):
        value |= (p[1 + i] & 0x7F) << (7 * i)
    return value


def OnInit():
    print("NoteForge Bridge ready")


def OnSysEx(event):
    p = _payload(event.sysex)
    if p is None or len(p) < 1:
        return
    command = p[0]
    if command in (1, 2) and len(p) >= 6:
        transport.setSongPos(_milliseconds(p), _MS)
        if command == 2 and not transport.isPlaying():
            transport.start()
    elif command == 3 and transport.isPlaying():
        transport.stop()
    event.handled = True
