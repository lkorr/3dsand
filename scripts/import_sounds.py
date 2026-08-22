#!/usr/bin/env python3
"""Convert whole-file takes into engine-ready sets.

The third cutter, and the one for takes that are ALREADY one event or one bed.
The other two exist because their sources hold many events that have to be
found and separated:

  split_footsteps.py  a walking performance, cuts follow detected onsets.
  split_breaks.py     a grid-authored take, cuts follow the 1 s grid.
  this script         the take IS the asset. No cutting -- only the format
                      conversion, the loudness pass, and (for loops) the seam.

Two kinds, chosen per entry in MAPPING:

ONE-SHOT (`kind='oneshot'`). A single event per file, several files forming the
variants of one set. Trimmed to the first and last non-silent sample so the cue
fires ON the attack -- leading silence in a one-shot is pure latency between
the visual and the sound -- then peak-matched across the variants so no single
take reads as a different, harder hit every time it comes up.

LOOP (`kind='loop'`). A bed meant to run indefinitely. The engine's loop path
(Voice::RenderAdd, the `!isOneShot` branch) wraps the playhead with a bare
`if (++pos >= size) pos = 0;` -- no crossfade, no envelope. So ANY seam has to
be baked into the file, and this script bakes it:

    body = take[:-X]                     # everything but the last X seconds
    body[:X] = body[:X]*sin + take[-X:]*cos

The tail is folded ON TOP of the head with an equal-power (sin/cos) pair and
the file is SHORTENED by the crossfade length. That is what makes the wrap
seamless: the last sample now continues naturally into the first, because the
material either side of the join is the same material summed at complementary
gains. Equal-power rather than linear because these beds are dense and
decorrelated -- a linear pair audibly dips ~3 dB through the middle of the
fade, which on a 5 s crossfade is a slow breath in the loop every cycle.

A fold is only correct when the take's head and tail are similar in level and
content; the script prints both RMS values and the residual step at the join so
that is checkable rather than assumed.

OUTPUT is what AudioLibrary::Rescan expects: mono 16-bit at the source rate,
named `<set>_NN.wav` under `assets/sounds/<prefix>/<set>/`. Mono because the
spatializer synthesizes the stereo image itself from the emitter position -- a
stereo asset arrives with its own baked image, which fights the panner and
smears the direction.

Usage:
    python scripts/import_sounds.py                # convert everything
    python scripts/import_sounds.py --dry-run      # report, write nothing
    python scripts/import_sounds.py --only mobs/dismember
"""

import argparse
import math
import os
import sys
import wave

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOUND_DIR = os.path.join(ROOT, "assets", "sounds")
RAW_DIR = os.path.join(SOUND_DIR, "raw")

# Output set -> how to build it.
#
#   srcs   source files in raw/, in variant order
#   kind   'oneshot' or 'loop'
#   xfade  loop only: crossfade seconds folded from the tail onto the head
#   use    loop only: (start, end) seconds of the source to loop, before the
#          fold. A take that DECAYS cannot be looped whole -- the quiet tail
#          crossfades against the dense head and the result pulses once per
#          cycle. Measured, not guessed: looping all 4.42 s of the blood take
#          swings 15.6 dB over four cycles, while 0..3.2 s swings 1.1 dB.
#
# The set name is the full path under assets/sounds/, i.e. exactly the string
# Cues resolves (prefix + '/' + the authored value). See assets/sound_schema.js.
MAPPING = {
    # One take per swing. Four variants is enough that a fight does not repeat
    # audibly, given the per-event pitch jitter on top.
    "mobs/dismember": {
        "kind": "oneshot",
        "srcs": ["dismember with sword 1.wav", "dismember with sword 2.wav",
                 "dismember with sword 3.wav", "dismember with sword 4.wav"],
    },
    # A wet, continuous dripping bed -- 20 drips in 4.4 s with overlapping
    # tails. Deliberately NOT split into individual drips: the density IS the
    # sound of bleeding heavily, and isolated drips would have to be re-stacked
    # by the engine to get it back. Looped, so a wound bleeds for as long as it
    # is bleeding rather than for 4.4 s.
    "gore/bleed": {
        "kind": "loop",
        "srcs": ["blood drip 1.wav"],
        # The take thins out after ~3 s as the drips space apart, so only the
        # dense head is looped. See `use` above for the measurement.
        "use": (0.0, 3.2),
        "xfade": 1.2,
    },
    # The night bed, 42 s folded to 39 s.
    #
    # THREE seconds, not the five this was first authored at, and the
    # difference is measurable: at 5 s the fold lands across the take's own
    # quiet passage (the bed ebbs to -43 dB around 8-10 s and again near the
    # end), both halves sit at ~70% through the middle of an equal-power fade,
    # and the sum reads -6 dB against the surrounding bed -- an audible breath
    # inward once per cycle. 3 s keeps the fold inside the denser material and
    # measures +0.03 dB. Lengthening it again means re-timing `use` too.
    "ambience/starlight": {
        "kind": "loop",
        "srcs": ["dark fairy atmosphere (starlight loop).wav"],
        "xfade": 3.0,
    },
}

SILENCE_DB = -60.0
# Padding left around a trimmed one-shot: enough that no attack is clipped by a
# sample-alignment error, short enough to add no audible latency.
HEAD_PAD_S = 0.004
TAIL_PAD_S = 0.050

FADE_IN_S = 0.0015
FADE_OUT_S = 0.030

PEAK_TARGET = 0.89
LOUDNESS_MATCH = 0.75


# ---------------------------------------------------------------------------
# I/O  (same shape as the sibling cutters; kept local so each runs standalone)
# ---------------------------------------------------------------------------

def read_wav(path):
    """Decode a wav to (float64 [n, channels] in -1..1, sample rate)."""
    with wave.open(path, "rb") as w:
        ch, sw, sr, n = (w.getnchannels(), w.getsampwidth(),
                         w.getframerate(), w.getnframes())
        raw = w.readframes(n)

    if sw == 2:
        data = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif sw == 3:
        # numpy has no int24: assemble little-endian, sign-extend from bit 23.
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v >= (1 << 23), v - (1 << 24), v)
        data = v.astype(np.float64) / float(1 << 23)
    elif sw == 4:
        data = np.frombuffer(raw, dtype="<i4").astype(np.float64) / float(1 << 31)
    else:
        raise RuntimeError("%s: unsupported sample width %d bytes" % (path, sw))

    return data.reshape(-1, ch), sr


def write_wav_mono16(path, samples, sr):
    """Write mono 16-bit PCM with TPDF dither (fixed seed: reruns are stable)."""
    x = np.clip(samples, -1.0, 1.0)
    rng = np.random.default_rng(0x5A17D17E)
    dither = (rng.random(x.shape) - rng.random(x.shape)) / 32768.0
    q = np.clip(np.rint((x + dither) * 32767.0), -32768, 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(q.tobytes())


# ---------------------------------------------------------------------------
# Shaping
# ---------------------------------------------------------------------------

def trim(mono, sr):
    """Strip leading/trailing silence, keeping a little padding either side."""
    a = np.abs(mono)
    gate = 10.0 ** (SILENCE_DB / 20.0)
    idx = np.nonzero(a > gate)[0]
    if len(idx) == 0:
        return mono, 0, len(mono)
    s = max(0, int(idx[0]) - int(sr * HEAD_PAD_S))
    e = min(len(mono), int(idx[-1]) + int(sr * TAIL_PAD_S))
    return mono[s:e], s, e


def fade_edges(x, sr):
    """De-click the ends of a one-shot."""
    n = len(x)
    fi = min(int(sr * FADE_IN_S), n // 2)
    fo = min(int(sr * FADE_OUT_S), n // 2)
    if fi > 0:
        x[:fi] *= np.linspace(0.0, 1.0, fi)
    if fo > 0:
        # Equal-power: a linear ramp audibly ducks through a noise-like tail.
        x[n - fo:] *= np.cos(np.linspace(0.0, math.pi / 2.0, fo))
    return x


def fold_loop(mono, sr, xfade_s):
    """Fold the last `xfade_s` onto the head so the file loops seamlessly.

    Returns (looped, report). The result is SHORTER than the input by the
    crossfade length -- that is inherent, not a bug: those samples have not
    been discarded, they have been summed into the head.
    """
    X = int(sr * xfade_s)
    if X <= 0 or len(mono) <= 2 * X:
        return mono, "crossfade skipped (take too short)"

    body = mono[:len(mono) - X].copy()
    tail = mono[len(mono) - X:]
    t = np.linspace(0.0, math.pi / 2.0, X)
    body[:X] = body[:X] * np.sin(t) + tail * np.cos(t)

    # Report the seam so a bad fold is visible rather than merely audible.
    seam = seam_anomaly(body, sr)
    step = abs(float(body[0]) - float(body[-1]))
    report = "seam %+.2f dB vs the bed, joint step %.5f" % (seam, step)
    if abs(seam) > 3.0:
        report += "  <-- AUDIBLE SEAM, try a different `use`/`xfade`"
    return body, report


def seam_anomaly(body, sr):
    """How much the wrap region stands out from the rest of the bed, in dB.

    Deliberately NOT a peak-to-trough spread over the whole loop: an ambient
    bed legitimately swells and ebbs (the starlight take moves 14 dB across 40
    seconds by design), and a metric that cannot tell composition from a defect
    would condemn every good loop.

    What a bad fold actually produces is a LOCAL anomaly at one point -- the
    join -- so this compares the level straddling the wrap against the median
    level of equal-length windows elsewhere. Near 0 dB means the join is
    indistinguishable from any other moment in the bed.
    """
    win = min(len(body) // 4, int(sr * 1.0))
    if win < int(sr * 0.05):
        return 0.0
    half = win // 2
    # The wrap: the end of the loop followed by its own beginning.
    wrap = np.concatenate([body[-half:], body[:half]])
    wrap_db = 20.0 * np.log10(np.sqrt((wrap ** 2).mean()) + 1e-12)
    # The bed: non-overlapping windows of the same length, away from the join.
    others = []
    for s in range(half, len(body) - win - half, win):
        seg = body[s:s + win]
        others.append(20.0 * np.log10(np.sqrt((seg ** 2).mean()) + 1e-12))
    if not others:
        return 0.0
    return float(wrap_db - np.median(others))


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def build_set(name, spec, dry_run):
    kind = spec["kind"]
    out_dir = os.path.join(SOUND_DIR, *name.split("/"))
    if not dry_run:
        os.makedirs(out_dir, exist_ok=True)

    leaf = name.split("/")[-1]
    prepared = []
    for src in spec["srcs"]:
        path = os.path.join(RAW_DIR, src)
        if not os.path.isfile(path):
            print("  !! missing source: %s" % path)
            continue
        data, sr = read_wav(path)
        mono = data.mean(axis=1)
        note = ""

        if kind == "oneshot":
            mono, s, e = trim(mono, sr)
            mono = fade_edges(mono, sr)
            note = "trimmed to %.3f-%.3f s" % (s / sr, e / sr)
        else:
            use = spec.get("use")
            if use:
                a = max(0, int(use[0] * sr))
                b = min(len(mono), int(use[1] * sr))
                mono = mono[a:b]
                note = "used %.2f-%.2f s; " % (use[0], use[1])
            fold_note = fold_loop(mono, sr, spec.get("xfade", 0.0))
            mono, tail_note = fold_note
            note += tail_note

        prepared.append((src, mono, sr, note))

    if not prepared:
        return 0

    # Peak-match across the variants of a set, blended so natural dynamics
    # survive. A loop is a single bed: matching it against nothing would just
    # normalize it, which is fine and is what LOUDNESS_MATCH gives here too.
    peaks = [float(np.abs(m).max()) or 1e-9 for _, m, _, _ in prepared]

    written = 0
    for i, ((src, mono, sr, note), p) in enumerate(zip(prepared, peaks)):
        x = mono * ((PEAK_TARGET / p) ** LOUDNESS_MATCH)
        m = float(np.abs(x).max())
        if m > 0.999:
            x *= 0.999 / m
        fname = "%s_%02d.wav" % (leaf, i + 1)
        dest = os.path.join(out_dir, fname)
        print("     %-16s %6.2fs peak %.3f -> %.3f  %s"
              % (fname, len(x) / sr, p, float(np.abs(x).max()), note))
        if not dry_run:
            write_wav_mono16(dest, x, sr)
        written += 1

    print("  %-20s %d file(s) -> assets/sounds/%s/" % (name, written, name))
    return written


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would be written, write nothing")
    ap.add_argument("--only", metavar="SET", help="build only this set")
    args = ap.parse_args()

    total = 0
    for name in sorted(MAPPING):
        if args.only and args.only != name:
            continue
        total += build_set(name, MAPPING[name], args.dry_run)

    print("%s %d files" % ("would write" if args.dry_run else "wrote", total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
