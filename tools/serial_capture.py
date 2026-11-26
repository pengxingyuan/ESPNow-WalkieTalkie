#!/usr/bin/env python3
"""Reset the board and capture its serial log for a few seconds."""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0

s = serial.Serial(port, 115200, timeout=0.2)
try:
    # classic auto-reset circuit: RTS -> EN, DTR -> IO0
    s.setRTS(True)
    s.setDTR(False)
    time.sleep(0.15)
    s.setRTS(False)
    time.sleep(0.05)
except Exception as e:  # noqa: BLE001
    print(f"[reset via DTR/RTS failed: {e}]")

end = time.time() + seconds
buf = bytearray()
while time.time() < end:
    try:
        chunk = s.read(2048)
    except Exception as e:  # noqa: BLE001
        print(f"[read error: {e}]")
        break
    if chunk:
        buf.extend(chunk)
s.close()

sys.stdout.buffer.write(bytes(buf))
print("\n--- captured", len(buf), "bytes ---")
