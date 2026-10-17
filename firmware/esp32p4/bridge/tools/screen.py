"""Sends keys, then prints the screen as text lines (non-empty rows only, compacted)."""
import serial, sys, time
keys = sys.argv[1].encode().decode('unicode_escape') if len(sys.argv) > 1 else ''
s = serial.Serial('/dev/cu.usbmodem5B5F0908891', 115200, timeout=0.1)
buf = b''
for k in keys:
    s.write(k.encode()); t = time.time()
    while time.time() - t < 0.6: buf += s.read(65536)
t = time.time()
while time.time() - t < 6: buf += s.read(65536)
txt = buf.decode(errors='replace').splitlines()
for l in txt:
    if l.startswith('[') and not l.startswith('[display]'): print(l[:160])
fr = [i for i, l in enumerate(txt) if l.startswith('[display] frame')]
if fr:
    rows = txt[fr[-1] + 2: fr[-1] + 34]
    print('\n'.join(r[:129] for r in rows if r.strip('| +-')))
else:
    print('(screen unchanged)')
