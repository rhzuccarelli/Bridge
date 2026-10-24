"""Measures how much of a recorded test tone reached the TP-7."""
import sys
import numpy as np
from load import load

def analyse(path, expect_hz=None):
    x, fs, ch, bits = load(path, info=True)
    level = np.sqrt(np.mean(x ** 2, axis=0))
    loudest = int(np.argmax(level))
    print(f"  format: {ch} ch, {fs} Hz, {bits}-bit; per-channel rms dB: "
          + ' '.join(f'{20*np.log10(l+1e-12):.0f}' for l in level))
    mono = x[:, loudest]
    n = len(mono)
    blk = fs // 1000
    blocks = n // blk  # 1 ms blocks
    present = np.array([np.max(np.abs(mono[b*blk:(b+1)*blk])) > 1e-4 for b in range(blocks)])
    sp = np.abs(np.fft.rfft(mono * np.hanning(n)))
    f0 = np.argmax(sp) * fs / n
    rms = 20 * np.log10(np.sqrt(np.mean(mono ** 2)) + 1e-12)
    peak = 20 * np.log10(np.max(np.abs(mono)) + 1e-12)
    # Clean-sine check: residual of a two-tap predictor at the dominant frequency.
    w = 2 * np.pi * f0 / fs
    resid = mono[2:] - 2 * np.cos(w) * mono[1:-1] + mono[:-2]
    glitches = int(np.sum(np.abs(resid) > 0.02))
    print(f"  {path.split('/')[-1]}: {n/fs:.2f} s, tone {f0:.0f} Hz, rms {rms:.1f} dB "
          f"(clean -18 dBFS sine = -21.0), peak {peak:.1f} dB, "
          f"tone present in {100*present.mean():.1f}% of 1 ms blocks, {glitches} glitch samples")
    line = ''.join('#' if p else '.' for p in present[:200])
    print(f"  first 200 ms: |{line}|")

if __name__ == '__main__':
    for p in sys.argv[1:]:
        analyse(p)
