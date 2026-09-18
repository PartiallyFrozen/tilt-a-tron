"""Capture the watch's USB serial console to a file (for catching panics).

    python tools/serial_capture.py COM4 build/serial.log 60
"""
import sys
import time

import serial

port, path, seconds = sys.argv[1], sys.argv[2], float(sys.argv[3])
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.2
s.dtr = False   # don't reset the chip on open
s.rts = False
s.open()
with s, open(path, "ab") as f:
    end = time.time() + seconds
    while time.time() < end:
        data = s.read(4096)
        if data:
            f.write(data)
            f.flush()

