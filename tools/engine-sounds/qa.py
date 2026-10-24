#!/usr/bin/env python3
"""Checks rendered takes: format, clipping, DC, spikes, steadiness, limp, brightness.

usage: uv run --no-project --with numpy python -I tools/engine-sounds/qa.py <out-root> <kind>...
       (or any python with numpy)

For every <out-root>/<kind>/<kind>_<rpm>_<on|off>.wav it prints one line. All
delivered takes pass the failure checks.

Failures (exit code 1):
  format   mono, 16-bit, 44.1 kHz, 3.00 s
  clip     a sample at full scale
  dc       |mean| >= 32 LSB (delivered takes are all under 13)
  spike    peak / 99.9th percentile > 1.25 (an isolated click would set the gain)

Reported, judged by ear (WARN):
  swing    RMS of 100 ms windows, max/min in dB: steadiness (idle reaches ~5 dB)
  limp     envelope modulation at the cycle rate (rpm / 120) minus that at the
           firing rate, in dB. Above 0, some cylinders are louder than others.
           Stock configs vary per-cylinder volume on purpose for character (r4,
           which the user liked, reaches +27 dB; v6 +31 dB). The original r6,
           three loud then three quiet pulses, was +2 dB on load and heard as
           "very clunky" together with its ringing IR. Use it to compare
           variants of one engine, not as a threshold.
  hf       energy above 1.5 kHz relative to the total, in dB: brightness (r4 is
           about -49 dB at mid rpm on load; the "metallic" first r6 was -25 dB)
"""
import sys
import wave
from pathlib import Path

import numpy as np

# Combustion pulses per crank revolution, for the firing rate
PULSES_PER_REV = {
    'r4': 2, 'r6': 3, 'r6b': 3, 'v6': 3, 'v8_diesel': 4,
    'flat2': 1, 'single_2t': 1, 'twin_2t': 2,
}


def check(path, pulses):
    w = wave.open(str(path))
    sr, ch, width, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
    x = np.frombuffer(w.readframes(n), dtype=np.int16).astype(float)
    rpm = int(path.stem.split('_')[-2])
    problems = []
    if (sr, ch, width, n) != (44100, 1, 2, 132300):
        problems.append(f'format {sr}/{ch}ch/{8 * width}bit/{n}')

    clipped = int(np.sum(np.abs(x) >= 32767))
    dc = abs(x.mean())
    spike = np.abs(x).max() / max(np.percentile(np.abs(x), 99.9), 1)
    win = sr // 10
    rms = [np.sqrt(np.mean(x[i:i + win] ** 2)) + 1e-9 for i in range(0, len(x) - win, win)]
    swing = 20 * np.log10(max(rms) / min(rms))

    cycle, firing = rpm / 120, rpm / 60 * pulses
    smooth = max(1, int(sr / (firing * 2)))
    env = np.convolve(np.abs(x), np.ones(smooth) / smooth, 'same')
    E = np.abs(np.fft.rfft((env - env.mean()) * np.hanning(len(env))))
    ef = np.fft.rfftfreq(len(env), 1 / sr)

    def at(f0):
        i = int(np.argmin(np.abs(ef - f0)))
        return E[max(0, i - 1):i + 2].max() + 1e-12

    limp = 20 * np.log10(at(cycle) / at(firing))

    X = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    fr = np.fft.rfftfreq(len(x), 1 / sr)
    hf = 10 * np.log10(np.sum(X[fr > 1500]) / np.sum(X[fr > 20]) + 1e-12)

    if clipped: problems.append(f'{clipped} clipped')
    if dc >= 32: problems.append(f'dc {dc:.1f}')
    if spike > 1.25: problems.append(f'spike {spike:.2f}')
    warn = ' (WARN limp)' if limp >= 0 else ''

    level = 20 * np.log10(np.sqrt(np.mean(x ** 2)) / 32767 + 1e-12)
    print(f'  {path.name:26s} rms {level:6.1f} dBFS  swing {swing:4.1f} dB  limp {limp:6.1f} dB  '
          f'hf {hf:6.1f} dB  {("OK" + warn) if not problems else "FAIL: " + ", ".join(problems)}')
    return not problems


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    root, ok = Path(sys.argv[1]), True
    for kind in sys.argv[2:]:
        print(kind)
        files = sorted((root / kind).glob(f'{kind}_*_o*.wav'), key=lambda p: (int(p.stem.split('_')[-2]), p.name))
        if not files:
            print('  no takes')
            ok = False
        for f in files:
            ok &= check(f, PULSES_PER_REV.get(kind, 2))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
