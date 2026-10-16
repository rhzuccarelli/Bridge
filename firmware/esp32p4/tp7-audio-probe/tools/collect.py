"""Reads the lab console, saves each take as a WAV, checks its CRC, analyses it."""
import base64, sys, time, zlib, serial
sys.path.insert(0, '.')
from analyse import analyse

port, seconds, outdir = sys.argv[1], float(sys.argv[2]), sys.argv[3]
s = serial.Serial(port, 2000000, timeout=0.5)
log = open(f"{outdir}/lab.log", "w")
end = time.time() + seconds
take = None
buf = b''
while time.time() < end:
    buf += s.read(65536)
    while b'\n' in buf:
        raw, buf = buf.split(b'\n', 1)
        line = raw.decode('latin1').rstrip('\r')
        if line.startswith('B64 ') and take is not None:
            take['data'].extend(base64.b64decode(line[4:]))
            continue
        log.write(line + '\n'); log.flush()
        if line.startswith('LABTAKE '):
            _, run, variant, size, name = line.split(' ', 4)
            take = {'run': run, 'variant': variant, 'name': name, 'data': bytearray()}
            print(f"run {run} variant {variant}: receiving {name} ({size} bytes)", flush=True)
        elif line.startswith('LABEND ') and take is not None:
            _, status, total, crc = line.split()
            data = bytes(take['data'])
            ok = len(data) == int(total) and zlib.crc32(data) == int(crc, 16)
            path = f"{outdir}/run{take['run']}_variant{int(take['variant'])+1}.wav"
            open(path, 'wb').write(data)
            print(f"  {status}, {len(data)} bytes, crc {'ok' if ok else 'MISMATCH'} -> {path}", flush=True)
            try:
                analyse(path)
            except Exception as e:
                print(f"  analysis failed: {e}", flush=True)
            take = None
        elif 'LAB' in line or ' E (' in line or 'detached' in line:
            print('  ' + line[-150:], flush=True)
