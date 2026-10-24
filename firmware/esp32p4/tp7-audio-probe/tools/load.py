import struct
import numpy as np

def load(path, info=False):
    """Returns float frames x channels from a PCM WAV (16/24/32-bit int or 32-bit float)."""
    f = open(path, 'rb').read()
    o, fmt, data = 12, None, None
    while o + 8 <= len(f):
        cid, size = f[o:o+4], struct.unpack('<I', f[o+4:o+8])[0]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', f[o+8:o+24])
        elif cid == b'data':
            data = f[o+8:o+8+size]
            break
        o += 8 + size + (size & 1)
    tag, ch, rate, _, align, bits = fmt
    if tag == 0xFFFE:
        tag = struct.unpack('<H', f[f.index(b'fmt ')+8+24:f.index(b'fmt ')+8+26])[0]
    width = bits // 8
    usable = len(data) // align * align
    raw = np.frombuffer(data[:usable], dtype=np.uint8).reshape(-1, width)
    if tag == 3:
        v = raw.copy().view('<f4').reshape(-1).astype(np.float64)
    else:
        v = np.zeros(len(raw), dtype=np.int64)
        for i in range(width):
            v |= raw[:, i].astype(np.int64) << (8 * i)
        v = np.where(v >= 1 << (bits - 1), v - (1 << bits), v) / float(1 << (bits - 1))
    out = v.reshape(-1, ch)
    if info:
        return out, rate, ch, bits
    return out
