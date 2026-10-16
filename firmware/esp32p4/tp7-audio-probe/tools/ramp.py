"""Counts the TP-7's overflow slips in a frame-numbering ramp recording."""
import sys
import numpy as np
from load import load

def slips(path):
    y, fs, ch, bits = load(path, info=True)
    L = y[:, 0]
    zero = L == 0
    n = np.round((L / 0.5 + 0.5) * 8192).astype(int)
    runs, i, N = [], 0, len(L)
    while i < N:
        j = i
        while j < N and zero[j] == zero[i]:
            j += 1
        runs.append((zero[i], i, j - i))
        i = j
    gaps = [(st, ln) for z, st, ln in runs if z and st > 0 and st + ln < N and ln > 8]
    extra = [((n[st + ln] - n[st - 1]) % 8192) - 2 * (ln + 1) for st, ln in gaps]
    secs = N / fs
    print(f"  {path.split('/')[-1]}: {secs:.2f} s, {len(gaps)} mutes ({len(gaps)/secs:.1f}/s), "
          f"muted {100*zero.mean():.1f}%, frames dropped per mute {sorted(set(extra))[:6]} "
          f"-> about {sum(extra)/secs:+.0f} frames/s too many")

for p in sys.argv[1:]:
    slips(p)
