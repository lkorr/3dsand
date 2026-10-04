#!/usr/bin/env python3
"""gen_elec_sounds.py -- PROCEDURAL PLACEHOLDER takes for the electricity and
weather sound sets (docs/PLAN_electricity_wave2.md, package E).

    python scripts/gen_elec_sounds.py            # writes the sets below
    python scripts/gen_elec_sounds.py --check    # regenerate in memory, compare

Writes, in the engine's sound format (mono, 16-bit PCM, 44.1 kHz WAV, one
set = one folder of takes, assets/sounds/<prefix>/<set>/<set>_NN.wav):

    weather/thunder   3 takes, ~3.5 s: a crack, then a low rolling rumble
    electric/zap      4 takes, ~0.3 s: an arc's crackle (sparse sharp clicks
                                       over a high buzz)
    electric/shock    3 takes, ~0.5 s: a body taking current (a mains-ish
                                       buzz, ragged, with crackle on top)

THESE ARE PLACEHOLDERS for the owner to replace with recorded takes: they are
here so the slots (audio/cues.cpp kSlotPrefix "thunder", "zap", "shock") are
audible in play rather than silent. Replace a set by dropping recorded WAVs
into its folder (and deleting these), or point the slot elsewhere.

DETERMINISTIC: the noise is a fixed-seed xorshift32 and every sample is plain
IEEE double arithmetic, so two runs (any machine) write byte-identical files.
--check proves it against what is on disk. Pure standard library.
"""
import math
import os
import struct
import sys
import wave

RATE = 44100
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "sounds")


class Rng:
    """xorshift32: deterministic, platform-independent."""

    def __init__(self, seed):
        # Neighbouring seeds (take 1, 2, 3) must not start neighbouring
        # streams: scramble, then run the generator in.
        s = ((seed * 2654435761) ^ (seed >> 15)) & 0xFFFFFFFF
        self.s = s or 0x9E3779B9
        for _ in range(16):
            self.u32()

    def u32(self):
        x = self.s
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.s = x & 0xFFFFFFFF
        return self.s

    def unit(self):  # [0, 1)
        return self.u32() / 4294967296.0

    def signed(self):  # [-1, 1)
        return self.unit() * 2.0 - 1.0


def one_pole_lp(xs, cutoff_hz):
    a = math.exp(-2.0 * math.pi * cutoff_hz / RATE)
    y, out = 0.0, []
    for x in xs:
        y = (1.0 - a) * x + a * y
        out.append(y)
    return out


def one_pole_hp(xs, cutoff_hz):
    lp = one_pole_lp(xs, cutoff_hz)
    return [x - l for x, l in zip(xs, lp)]


def normalize(xs, peak=0.89):
    m = max(1e-9, max(abs(x) for x in xs))
    return [x * peak / m for x in xs]


def fade_edges(xs, ms_in=2.0, ms_out=20.0):
    n = len(xs)
    a, b = int(RATE * ms_in / 1000.0), int(RATE * ms_out / 1000.0)
    out = list(xs)
    for i in range(min(a, n)):
        out[i] *= i / a
    for i in range(min(b, n)):
        out[n - 1 - i] *= i / b
    return out


def thunder(seed):
    r = Rng(seed)
    n = int(RATE * (3.2 + 0.6 * r.unit()))
    white = [r.signed() for _ in range(n)]
    # The crack: bright noise, a sharp ~60 ms decay.
    crack_hp = one_pole_hp(white, 1500.0)
    # The rumble: brown-ish noise (two low-passes), swelling in 3-5 lumps.
    rum = one_pole_lp(one_pole_lp(white, 180.0), 90.0)
    lumps = [(0.15 + 0.5 * r.unit() * k, 0.25 + 0.35 * r.unit(), 0.5 + 0.5 * r.unit())
             for k in range(1, 5)]
    out = []
    for i in range(n):
        t = i / RATE
        crack = crack_hp[i] * math.exp(-t / 0.06) * 1.4
        env = math.exp(-t / 1.4)
        for (c, w, g) in lumps:
            env += g * math.exp(-((t - c) / w) ** 2) * math.exp(-t / 2.2)
        out.append(crack + rum[i] * env * 9.0)
    return fade_edges(normalize(out), 1.0, 300.0)


def zap(seed):
    r = Rng(seed)
    n = int(RATE * (0.24 + 0.12 * r.unit()))
    out = [0.0] * n
    # Sparse sharp clicks: short decaying bursts at random times, denser early.
    t = 0.0
    while True:
        t += 0.002 + 0.02 * r.unit() * (1.0 + 4.0 * t)
        i0 = int(t * RATE)
        if i0 >= n:
            break
        amp = (0.4 + 0.6 * r.unit()) * math.exp(-t / 0.15)
        for k in range(int(RATE * 0.004)):
            if i0 + k >= n:
                break
            out[i0 + k] += amp * r.signed() * math.exp(-k / (RATE * 0.0008))
    # A high buzz under it (an arc's hum), ragged in pitch.
    f, ph = 1900.0 + 600.0 * r.unit(), 0.0
    for i in range(n):
        tt = i / RATE
        ph += 2.0 * math.pi * (f * (1.0 + 0.05 * r.signed())) / RATE
        out[i] += 0.12 * (1.0 if math.sin(ph) > 0 else -1.0) * math.exp(-tt / 0.12)
    out = one_pole_hp(out, 600.0)
    return fade_edges(normalize(out), 0.5, 15.0)


def shock(seed):
    r = Rng(seed)
    n = int(RATE * (0.45 + 0.15 * r.unit()))
    base = 110.0 + 30.0 * r.unit()
    out, ph = [], 0.0
    jitter = one_pole_lp([r.signed() for _ in range(n)], 30.0)
    for i in range(n):
        t = i / RATE
        ph += 2.0 * math.pi * base * (1.0 + 0.6 * jitter[i]) / RATE
        # A buzz rich in odd harmonics (a clipped sine), amplitude-ragged.
        s = math.sin(ph)
        buzz = max(-0.6, min(0.6, 1.8 * s)) + 0.25 * math.sin(3.0 * ph) + 0.12 * math.sin(7.0 * ph)
        env = min(1.0, t / 0.01) * (0.7 + 0.3 * math.sin(2.0 * math.pi * 13.0 * t)) * math.exp(-t / 0.35)
        out.append(buzz * env)
    # Crackle on top.
    for _ in range(40):
        i0 = int(r.unit() * n)
        amp = 0.5 * r.unit()
        for k in range(int(RATE * 0.003)):
            if i0 + k < n:
                out[i0 + k] += amp * r.signed() * math.exp(-k / (RATE * 0.0006))
    out = one_pole_hp(out, 70.0)
    return fade_edges(normalize(out), 1.0, 40.0)


SETS = [
    ("weather", "thunder", thunder, 3, 0x7A0D0000),
    ("electric", "zap", zap, 4, 0x2A900000),
    ("electric", "shock", shock, 3, 0x5C0C0000),
]


def wav_bytes(samples):
    frames = b"".join(struct.pack("<h", max(-32767, min(32767, int(round(x * 32767.0)))))
                      for x in samples)
    import io
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(frames)
    return buf.getvalue()


def main():
    check = "--check" in sys.argv
    bad = 0
    for prefix, name, fn, takes, seed in SETS:
        d = os.path.join(ROOT, prefix, name)
        if not check:
            os.makedirs(d, exist_ok=True)
        for k in range(1, takes + 1):
            data = wav_bytes(fn(seed + k))
            path = os.path.join(d, "%s_%02d.wav" % (name, k))
            if check:
                same = os.path.exists(path) and open(path, "rb").read() == data
                print("%s %s" % ("ok  " if same else "DIFF", os.path.relpath(path, ROOT)))
                bad += 0 if same else 1
            else:
                with open(path, "wb") as f:
                    f.write(data)
                print("wrote %s (%.2f s)" % (os.path.relpath(path, ROOT),
                                              (len(data) - 44) / 2.0 / RATE))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
