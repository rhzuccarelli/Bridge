"""Waits until the lab firmware starts a cycle (TP-7 in audio mode), so a flash cannot cut an MTP transfer."""
import sys, time, serial
s = serial.Serial(sys.argv[1], 2000000, timeout=0.5)
end = time.time() + float(sys.argv[2])
buf = b''
while time.time() < end:
    buf += s.read(4096)
    if b'LAB run' in buf:
        print("safe: cycle started"); sys.exit(0)
    buf = buf[-200:]
print("timeout"); sys.exit(1)
