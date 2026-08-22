#!/usr/bin/env python3
"""Cut a GRID-ALIGNED performance into one .wav per event.

The sibling of split_footsteps.py, and deliberately a separate script because
the two takes are different KINDS of recording and want opposite treatment.

  split_footsteps.py  a human walking, ~0.5 s apart, drifting off the grid by
                      up to 80 ms. Cuts must FOLLOW the performance, so it runs
                      spectral-flux onset detection and back-off.

  this script         a take authored one event per second, on the second. The
                      grid IS the edit. Detecting onsets here would be strictly
                      worse: `branch breaks 6s.wav` has events whose loudest
                      frame lands 440 ms after the attack (a snap, then the
                      branch hits the ground), and a flux picker happily
                      promotes that second thump to its own "event", splitting
                      one break into two half-breaks that both sound broken.

So: slice on the grid, verify the grid was right, and only shape the edges.

WHAT IS STILL DONE PER SLICE, because a raw grid cut is not a finished asset:
  - The onset is nudged to the last silent sample before the attack, searching
    only a few ms around the boundary. A break recorded 1.8 ms late keeps that
    1.8 ms as pre-roll otherwise, and pre-roll on a one-shot is latency between
    the visual and the sound.
  - A slice whose tail is still ringing at the boundary gets an equal-power
    out-fade. Blocks that end in true digital silence are left completely
    alone -- fading silence is a no-op that only risks touching a real tail.
  - Peak matching toward PEAK_TARGET, blended by LOUDNESS_MATCH, same as the
    footstep cutter and for the same reason: a variant that is much louder than
    its siblings reads as a different event every time it comes up, but
    perfectly uniform variants read as synthetic.

OUTPUT is what AudioLibrary::Rescan expects: mono 16-bit at the source rate,
named `<set>_NN.wav` under `assets/sounds/<prefix>/<set>/`. Mono because the
spatializer synthesizes the stereo image from the emitter position -- a stereo
asset's own image fights the panner and smears the direction.

Usage:
    python scripts/split_breaks.py                 # cut everything in MAPPING
    python scripts/split_breaks.py --dry-run       # report cuts, write nothing
    python scripts/split_breaks.py --only branch
"""

import argparse
import math
import os
import sys
import wave

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOUND_DIR = os.path.join(ROOT, "assets", "sounds")
# Sources live in raw/, which AudioLibrary::ScanDir skips by directory name --
# an 11-second take decoded as a "set" would hold megabytes nothing can trigger.
RAW_DIR = os.path.join(SOUND_DIR, "raw")

# source file (relative to assets/sounds/) -> (output prefix, set name, period)
#
# `period` is the authored grid in seconds. The output set name is what a
# material's "sounds" block refers to: prefix "breaks" + set "branch" resolves
# to the set `breaks/branch`, which is the concatenation Cues::kSlotPrefix does
# at runtime -- see assets/sound_schema.js.
#
# NOTE the "6s" in the filename. raw/ already held an ELEVEN second
# `branch breaks.wav` -- the deadfall-underfoot take that split_footsteps.py
# cuts into footsteps/branch/. This is a different, later recording of actual
# breaks on a 1 s grid, and the two must not share a name: the footstep cutter
# reads the other one by name from the same folder.
MAPPING = {
    "branch breaks 6s.wav": ("breaks", "branch", 1.0),
}

# How far around a grid boundary to hunt for the true start of the attack. The
# take is accurate to ~2 ms; this is generous enough to absorb a sloppier one
# without ever reaching into the neighbouring event.
SNAP_WINDOW_S = 0.050

# A sample is "silence" below this. -60 dBFS is safely under the noise floor of
# these takes and safely above the -138 dBFS of true digital zero.
SILENCE_DB = -60.0

# Leave this much silence in front of the transient: enough that no attack is
# clipped by a sample-alignment error, short enough to add no audible latency.
PRE_ROLL_S = 0.004

# How long a quiet stretch must last to count as a GAP BETWEEN events rather
# than a dip within one. A branch break crackles as it decays, so it dips below
# the silence gate repeatedly; 15 ms is longer than any of those dips and far
# shorter than the real gaps in the take (which run 100 ms+).
MIN_GAP_S = 0.015

FADE_IN_S = 0.0015
FADE_OUT_S = 0.030

# Only fade out slices still ringing at the cut. A block ending below this is
# already silent and is written through untouched.
RING_DB = -70.0

PEAK_TARGET = 0.89
LOUDNESS_MATCH = 0.75


# ---------------------------------------------------------------------------
# I/O  (shared shape with split_footsteps.py; kept local so either script can
# be run or edited without the other)
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
# Cutting
# ---------------------------------------------------------------------------

def snap_start(a, sr, boundary):
    """Last silent sample before the attack nearest `boundary`.

    Searches +-SNAP_WINDOW_S. Walks FORWARD to the first sample above the
    silence gate, then BACKWARD to the last one below it, so a boundary that
    already sits inside the attack still backs out to its foot. Returns the
    boundary unchanged if the window is entirely silent or entirely loud --
    both mean the grid is the best guess available.
    """
    gate = 10.0 ** (SILENCE_DB / 20.0)
    w = int(sr * SNAP_WINDOW_S)
    lo = max(0, boundary - w)
    hi = min(len(a), boundary + w)
    seg = a[lo:hi]
    if len(seg) == 0:
        return boundary

    above = np.nonzero(seg > gate)[0]
    if len(above) == 0 or len(above) == len(seg):
        return boundary

    first = int(above[0])
    # Back off to the last sub-gate sample before that attack.
    i = first
    while i > 0 and seg[i] > gate:
        i -= 1
    return int(max(lo, lo + i - int(sr * PRE_ROLL_S)))


def start_of_next(a, sr, boundary, n_blocks, i, total):
    """Where the next slice begins -- which is where this one ends.

    Two cases, and telling them apart is the whole point:

      QUIET BOUNDARY. The event decayed into silence before the grid line, so
      there is a real gap. Cut at the far end of that gap, i.e. immediately
      before the next attack, and this slice keeps its entire natural tail.

      RINGING BOUNDARY. The event is still sounding when the next one starts
      (blocks 5 and 6 of `branch breaks.wav` do exactly this). There is no
      correct cut: any split divides a continuous signal. Take the grid line
      itself -- the authored intent -- and let the out-fade shape it. Hunting
      for a quiet moment here is what pulls the cut backwards INTO the tail.
    """
    if i + 1 >= n_blocks:
        return total
    gate = 10.0 ** (SILENCE_DB / 20.0)
    w = int(sr * SNAP_WINDOW_S)
    lo = max(0, boundary - w)
    hi = min(total, boundary + w)
    seg = a[lo:hi]
    if len(seg) == 0:
        return boundary

    # Find the next attack: a silence->loud transition whose silence lasted at
    # least MIN_GAP_S. That minimum is what distinguishes a real gap between
    # two events from a momentary dip inside one decaying tail -- a branch
    # break is not a smooth decay, it crackles, and a plain transition test
    # fires on the first crackle and truncates the event.
    loud = seg > gate
    quiet_run = 0
    need = int(sr * MIN_GAP_S)
    for j in range(len(seg)):
        if loud[j]:
            if quiet_run >= need and j > 0:
                return int(lo + j - int(sr * PRE_ROLL_S))
            quiet_run = 0
        else:
            quiet_run += 1
    # No qualifying gap: either silent throughout (nothing to protect) or
    # ringing throughout (no correct cut exists). The grid is the answer.
    return boundary


def apply_fades(x, sr, fade_out):
    n = len(x)
    fi = min(int(sr * FADE_IN_S), n // 2)
    if fi > 0:
        x[:fi] *= np.linspace(0.0, 1.0, fi)
    if fade_out:
        fo = min(int(sr * FADE_OUT_S), n // 2)
        if fo > 0:
            # Equal-power: a linear ramp audibly ducks through a noise tail.
            x[n - fo:] *= np.cos(np.linspace(0.0, math.pi / 2.0, fo))
    return x


def split_file(src_path, prefix, set_name, period, dry_run):
    data, sr = read_wav(src_path)
    mono = data.mean(axis=1)
    a = np.abs(mono)

    step = int(round(period * sr))
    n_blocks = len(mono) // step
    if n_blocks == 0:
        print("  !! %s is shorter than one %.2fs block" %
              (os.path.basename(src_path), period))
        return 0

    ring = 10.0 ** (RING_DB / 20.0)
    gate = 10.0 ** (SILENCE_DB / 20.0)

    slices = []
    for i in range(n_blocks):
        b0 = i * step
        b1 = (i + 1) * step
        start = snap_start(a, sr, b0)
        # Never let a snap reach back into the previous slice.
        if slices and start < slices[-1][1] - int(sr * FADE_OUT_S):
            start = b0
        # The slice runs to where the NEXT one starts, so no audio is dropped
        # and none is duplicated. Deliberately not snap_start(b1): that hunts
        # the next ATTACK, and when this slice is still ringing at the boundary
        # the hunt lands in its own decaying tail -- which both truncates this
        # event and grafts 50 ms of it onto the front of the next file.
        end = start_of_next(a, sr, b1, n_blocks, i, len(mono))
        end = max(end, start + int(sr * 0.05))
        # An empty block is a hole in the take, not an asset.
        if a[start:end].max() < gate:
            print("     block %d is silent, skipped" % (i + 1))
            continue
        slices.append((start, end))

    out_dir = os.path.join(SOUND_DIR, prefix, set_name)
    if not dry_run:
        os.makedirs(out_dir, exist_ok=True)

    peaks = [float(a[s:e].max()) or 1e-9 for s, e in slices]
    written = 0
    for i, ((s, e), p) in enumerate(zip(slices, peaks)):
        gain = (PEAK_TARGET / p) ** LOUDNESS_MATCH
        x = mono[s:e].copy() * gain
        # Fade only a slice that is still sounding at its cut; one that ended
        # in silence is written through with its tail exactly as recorded.
        fade_out = float(np.abs(x[-int(sr * 0.005):]).max()) > ring
        x = apply_fades(x, sr, fade_out)
        m = float(np.abs(x).max())
        if m > 0.999:
            x *= 0.999 / m

        name = "%s_%02d.wav" % (set_name, i + 1)
        if dry_run:
            print("     %-14s %6.3f -> %6.3f  (%5.0f ms, peak %.3f -> %.3f%s)"
                  % (name, s / sr, e / sr, (e - s) * 1000.0 / sr, p,
                     float(np.abs(x).max()), ", faded" if fade_out else ""))
        else:
            write_wav_mono16(os.path.join(out_dir, name), x, sr)
        written += 1

    print("  %-8s %2d events  ->  assets/sounds/%s/%s/"
          % (set_name, written, prefix, set_name))
    return written


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true",
                    help="print the cuts that would be made, write nothing")
    ap.add_argument("--only", metavar="SET", help="cut only this output set")
    args = ap.parse_args()

    total = 0
    for src, (prefix, name, period) in sorted(MAPPING.items(),
                                              key=lambda kv: kv[1][1]):
        if args.only and args.only != name:
            continue
        path = os.path.join(RAW_DIR, src)
        if not os.path.isfile(path):
            print("  !! missing source: %s" % path)
            continue
        total += split_file(path, prefix, name, period, args.dry_run)

    print("%s %d files" % ("would write" if args.dry_run else "wrote", total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
