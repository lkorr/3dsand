"""Generate PLACEHOLDER combat sounds: assets/sounds/melee/{whoosh,flesh,clang}.

THESE ARE PLACEHOLDERS AND THEY SOUND LIKE IT. Every other set in this project
is a real recording, cut by scripts/import_sounds.py from a take in
assets/sounds/raw/ — "the take IS the asset" is that file's whole thesis and it
is the right one. Nothing here is arguing with it. But the three melee slots
landed with no recordings behind them, and a slot with no asset is INVISIBLE:
Cues::Combat resolves the set, gets -1, and returns silently, so a wiring bug
and an empty folder are the same observation from the speakers. Synthesised
stand-ins make the wiring audible and give the mix something to be judged
against.

WHEN THE REAL TAKES ARRIVE, DELETE THIS SCRIPT. Add the sources to
assets/sounds/raw/, put three entries in import_sounds.py's MAPPING
("melee/whoosh", "melee/flesh", "melee/clang", kind "oneshot"), and let the
normal pipeline overwrite these files. Nothing in the engine has to change: the
library scans folders, so a set with better files in it is simply a better set.

WHAT EACH ONE IS TRYING TO BE (see assets/sound_schema.js for what fires them):

  whoosh  air moving past an edge. Band-passed noise whose centre frequency
          SWEEPS UP and then falls, which is what makes a whoosh read as a
          thing passing rather than as a hiss. Short, because the cut is.
  flesh   the blow, not the wound. A low body thump under a short wet noise
          burst; deliberately dull, since `dismember` is the wet one and these
          two have to stay tellable apart.
  clang   steel stopping steel. INHARMONIC partials (the ratios below are not
          integers on purpose — a harmonic stack is a bell, a struck plate is
          not) over a bright transient, with the high partials decaying first.

DETERMINISTIC. The noise is a seeded LCG rather than `random`, so re-running
this rewrites byte-identical files and a regeneration never shows up as a diff.

Format matches every other asset in the project: MONO, 16-bit PCM, 44.1 kHz
(the library resamples to the device rate at load, so the file rate is not
load-bearing — see scripts/split_footsteps.py's docstring).

Run: python scripts/gen_combat_sounds.py
"""

import math
import os
import struct
import wave

RATE = 44100
PEAK_TARGET = 0.89        # same headroom import_sounds.py leaves
VARIANTS = 3              # per set: enough that a flurry does not machine-gun


class Rng:
    """A seeded LCG. Not `random` — this file must be byte-reproducible across
    Python versions, and `random`'s internals are not part of its contract."""

    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def next_u32(self):
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.s

    def bipolar(self):
        """White noise in [-1, 1)."""
        return self.next_u32() / 2147483648.0 - 1.0


class OnePole:
    """A one-pole lowpass, used in pairs to make a crude bandpass. Enough shape
    for a placeholder; a real take will not need any of this."""

    def __init__(self):
        self.z = 0.0

    def run(self, x, cutoff_hz):
        # Standard one-pole coefficient. Clamped because the sweeps below run
        # the cutoff up near Nyquist, where the approximation falls apart.
        c = min(max(cutoff_hz, 20.0), RATE * 0.45)
        a = 1.0 - math.exp(-2.0 * math.pi * c / RATE)
        self.z += a * (x - self.z)
        return self.z


def fade_edges(buf, in_ms=1.5, out_ms=30.0):
    """The same edge treatment import_sounds.py applies: a very short fade in
    (kills the click of starting mid-waveform) and an equal-power fade out."""
    n = len(buf)
    ni = min(int(RATE * in_ms / 1000.0), n // 2)
    no = min(int(RATE * out_ms / 1000.0), n // 2)
    for i in range(ni):
        buf[i] *= i / ni
    for i in range(no):
        # Equal power, not linear: a linear fade on a noisy tail audibly dips.
        buf[n - 1 - i] *= math.sqrt(i / no)
    return buf


def normalize(buf, target=PEAK_TARGET):
    peak = max((abs(v) for v in buf), default=0.0)
    if peak < 1e-9:
        return buf
    k = target / peak
    return [v * k for v in buf]


def whoosh(seed, seconds=0.26):
    """Band-passed noise with a rising-then-falling centre frequency."""
    rng = Rng(seed)
    lo, hi = OnePole(), OnePole()
    n = int(RATE * seconds)
    out = []
    for i in range(n):
        t = i / n
        # The sweep: up fast, down slower. `sin(pi*t)` peaks in the middle,
        # which is the point the blade is closest to the ear.
        centre = 420.0 + 2600.0 * math.sin(math.pi * t) ** 1.4
        x = rng.bipolar()
        band = lo.run(x, centre * 1.9) - hi.run(x, centre * 0.55)
        # Envelope: quick swell, exponential release. Not a click — air has no
        # attack transient, which is exactly what separates this from an impact.
        env = math.sin(math.pi * t) ** 1.8
        out.append(band * env)
    return fade_edges(normalize(out), in_ms=4.0, out_ms=45.0)


def flesh(seed, seconds=0.22):
    """A low body thump under a short damp noise burst."""
    rng = Rng(seed)
    damp = OnePole()
    n = int(RATE * seconds)
    out = []
    # The thump's pitch falls through the hit, which is what makes it read as
    # something soft absorbing the blow rather than as a drum.
    f0, f1 = 118.0, 62.0
    phase = 0.0
    for i in range(n):
        t = i / n
        f = f0 + (f1 - f0) * t
        phase += 2.0 * math.pi * f / RATE
        body = math.sin(phase) * math.exp(-7.0 * t)
        # The wet part: noise rolled off hard, so it is a slap and not a hiss.
        wet = damp.run(rng.bipolar(), 900.0 - 500.0 * t) * math.exp(-26.0 * t)
        out.append(body * 0.85 + wet * 0.9)
    return fade_edges(normalize(out), in_ms=0.4, out_ms=25.0)


def clang(seed, seconds=0.52):
    """Inharmonic partials over a bright transient; highs decay first."""
    rng = Rng(seed)
    bright = OnePole()
    n = int(RATE * seconds)
    # NOT integer ratios. A harmonic stack is a bell or a pipe; a struck steel
    # plate has modes that do not line up, and the beating between them is most
    # of what makes it sound like metal.
    base = 1180.0 + (seed % 7) * 34.0
    ratios = [1.0, 1.73, 2.41, 3.19, 4.62, 5.87]
    decays = [4.5, 6.5, 8.5, 11.0, 15.0, 19.0]
    out = []
    for i in range(n):
        t = i / n
        v = 0.0
        for r, d in zip(ratios, decays):
            # Amplitude falls with the partial index so the fundamental carries
            # the pitch and the rest are colour.
            v += math.sin(2.0 * math.pi * base * r * (i / RATE)) * \
                math.exp(-d * t) / (r * 1.4)
        # The strike itself: two milliseconds of bright noise. Without it the
        # partials fade in and it sounds like a synth pad, not a hit.
        v += bright.run(rng.bipolar(), 6000.0) * math.exp(-140.0 * t) * 1.1
        out.append(v)
    return fade_edges(normalize(out), in_ms=0.3, out_ms=60.0)


def write_wav_mono16(path, buf):
    """Mono, 16-bit, RATE Hz — the format every other asset here is in.

    TPDF dither with a FIXED seed, exactly as import_sounds.py does it and for
    the same reason: dither removes quantisation distortion on the tail, and a
    fixed seed keeps reruns byte-stable so a regeneration is never a diff."""
    d = Rng(0x5A17D17E)
    frames = bytearray()
    for v in buf:
        # TPDF = the sum of two uniform draws, which is what makes the noise
        # floor flat instead of correlated with the signal.
        dith = (d.bipolar() + d.bipolar()) * 0.5 / 32768.0
        s = int(max(-1.0, min(1.0, v + dith)) * 32767.0)
        frames += struct.pack("<h", s)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))
    return len(buf)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_root = os.path.join(root, "assets", "sounds", "melee")

    # Set name == slot name. Cues::CombatSetId builds "melee/<slot>" from
    # kSlotPrefix, so these folder names are not a choice made here — renaming
    # one silently unbinds the slot.
    sets = [("whoosh", whoosh, 0x57007), ("flesh", flesh, 0xF1E58),
            ("clang", clang, 0xC1A46)]

    for name, gen, seed in sets:
        for k in range(VARIANTS):
            buf = gen(seed + k * 7919)
            path = os.path.join(out_root, name, f"{name}_{k + 1:02d}.wav")
            n = write_wav_mono16(path, buf)
            print(f"wrote {path}  ({n} frames, {n / RATE:.3f}s)")

    print("\nPLACEHOLDERS. See this file's docstring for how to replace them "
          "through the normal scripts/import_sounds.py pipeline.")


if __name__ == "__main__":
    main()
