#!/usr/bin/env python3
"""Cut a recording of evenly-spaced footsteps into one .wav per step.

The source recordings in assets/sounds/raw/ are performances: someone walking on
a surface, roughly one step every 0.5 s, recorded as one long 24-bit stereo
take. The game wants the opposite shape -- a folder of short mono one-shots that
AudioLibrary can pick a random variant from per step. This script is the
bridge, and it is deliberately a SCRIPT and not an engine feature: cutting is
done once, by ear, and the results are checked into assets/sounds/footsteps/.

WHY NOT JUST SLICE ON A 0.5 s GRID. The performances are human, so the steps
drift off the nominal grid by up to ~80 ms, and a fixed grid puts the cut
somewhere in the middle of the attack -- the single most audible place to cut.
A step whose transient is clipped reads as a different, weaker material. So:

  1. Onset detection by SPECTRAL FLUX (positive-only, per STFT frame). Flux
     spikes on the broadband noise burst of a foot hitting a surface, which is
     what a footstep IS. A plain amplitude envelope mis-fires here: leaf and
     branch steps have a slow rustling decay that is nearly as loud as the
     attack, so the envelope has no clear peak, but the flux does.
  2. Peak-picking with a refractory window, seeded from the nominal spacing so
     a step's own secondary detail (heel-then-toe, a twig settling) cannot be
     promoted to a separate step.
  3. BACK OFF from the detected onset to where the signal actually leaves the
     local noise floor, then subtract a fixed pre-roll. The cut therefore lands
     in silence just BEFORE the transient -- the user's requirement, and the
     only cut that preserves the attack intact.
  4. End the slice at the next step's cut point (or at decay into the floor,
     whichever is first) so tails ring out naturally instead of being chopped
     at a fixed length.

OUTPUT FORMAT is what AudioLibrary::Rescan expects: mono, 16-bit, at the source
rate; named `<set>_NN.wav` inside `footsteps/<set>/`. The library resamples to
the device rate at load, so the file rate here is not load-bearing. Mono because the
spatializer needs a mono source -- it synthesizes the stereo image itself from
the emitter position, so a stereo asset's own image would fight the panner and
smear the direction. Downmix happens HERE, not at load, so what is checked in
is exactly what plays.

Usage:
    python scripts/split_footsteps.py                  # cut everything in MAPPING
    python scripts/split_footsteps.py --dry-run        # report cuts, write nothing
    python scripts/split_footsteps.py --only metal     # one set
"""

import argparse
import math
import os
import struct
import sys
import wave

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOUND_DIR = os.path.join(ROOT, "assets", "sounds")
RAW_DIR = os.path.join(SOUND_DIR, "raw")
OUT_ROOT = os.path.join(SOUND_DIR, "footsteps")

# source file (in assets/sounds/raw/) -> output set name (folder under footsteps/)
#
# The set name is what materials.json's `footstep` field refers to, so renaming
# a folder here means editing that field too. `branch breaks` is not a walking
# surface -- it is the snap layer for stepping on deadfall -- but it is cut the
# same way and lives alongside the others so the debris one-shots share the
# library's variant machinery.
MAPPING = {
    "path footsteps.wav": "path",
    "leaf footsteps.wav": "leaf",
    "metal footsteps.wav": "metal",
    "branch breaks.wav": "branch",
}

# Nominal spacing of the performance, seconds. Only used to seed the refractory
# window -- actual cuts follow the detected onsets, not this.
NOMINAL_SPACING = 0.5

# Silence to leave in front of the transient. Long enough that no attack is
# ever clipped by a sample-alignment error, short enough that it does not add
# audible latency to a step (a footstep must land ON the foot plant).
PRE_ROLL_S = 0.012

# Fades applied at the slice edges. 1.5 ms in is inaudible on a noise burst but
# guarantees no DC step-click; the out-fade is longer because it lands in a
# decaying tail where a hard cut is much more obvious.
FADE_IN_S = 0.0015
FADE_OUT_S = 0.030

# A slice ends when its RMS has fallen this far below its own peak, so tails
# are kept in proportion to how loud the step was.
TAIL_FLOOR_DB = -46.0

# Hard bounds on a slice, seconds. The max stops a missed onset from welding
# two steps into one asset; the min discards detector noise.
MIN_LEN_S = 0.06
MAX_LEN_S = 0.90

# Per-variant peak normalization target. Steps are normalized so that no single
# variant is freakishly louder than its siblings (which would read as a stumble
# every time that variant came up), but only up to LOUDNESS_MATCH: beyond that
# the natural dynamics of the performance are kept, because perfectly uniform
# footsteps sound synthetic. 0 = no matching, 1 = every step identical peak.
PEAK_TARGET = 0.89
LOUDNESS_MATCH = 0.75

FFT_N = 1024
HOP = 256


# ---------------------------------------------------------------------------
# I/O
# ---------------------------------------------------------------------------

def read_wav(path):
    """Decode a wav to (float64 [n, channels] in -1..1, sample rate).

    Handles 16- and 24-bit PCM, which is all the source material uses. The
    stdlib `wave` module hands back raw frames, so 24-bit is unpacked by hand:
    numpy has no int24 dtype, so the three bytes are assembled little-endian
    and sign-extended from bit 23.
    """
    with wave.open(path, "rb") as w:
        ch, sw, sr, n = (w.getnchannels(), w.getsampwidth(),
                         w.getframerate(), w.getnframes())
        raw = w.readframes(n)

    if sw == 2:
        data = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif sw == 3:
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
    """Write mono 16-bit PCM with TPDF dither.

    Dither matters more here than the file size suggests: these slices are
    quiet (a footstep tail decays to near nothing) and get amplified by the
    distance-gain curve when the emitter is close, so plain truncation would
    put correlated quantization noise right where the ear is listening.
    """
    x = np.clip(samples, -1.0, 1.0)
    # TPDF = sum of two independent uniforms, +-1 LSB total. Decorrelates the
    # quantization error from the signal at the cost of ~+4.8 dB noise floor.
    rng = np.random.default_rng(0x5A17D17E)  # fixed: reruns are byte-identical
    dither = (rng.random(x.shape) - rng.random(x.shape)) / 32768.0
    q = np.clip(np.rint((x + dither) * 32767.0), -32768, 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(q.tobytes())


# ---------------------------------------------------------------------------
# Analysis
# ---------------------------------------------------------------------------

def spectral_flux(mono, sr):
    """Positive spectral flux per frame, plus the frame hop in samples.

    Flux = sum over bins of max(0, |X_t| - |X_{t-1}|): it measures how much
    energy APPEARED this frame. A footstep is a broadband burst, so it lights
    up flux across the whole spectrum at once, while the rustle of a tail
    redistributes energy without adding much -- which is exactly the
    discrimination an amplitude envelope fails to make on leaves.
    """
    win = np.hanning(FFT_N)
    n_frames = max(0, 1 + (len(mono) - FFT_N) // HOP)
    if n_frames <= 0:
        return np.zeros(0), HOP

    # Frame the signal as a strided view, window, then one batched rFFT.
    idx = np.arange(FFT_N)[None, :] + HOP * np.arange(n_frames)[:, None]
    mag = np.abs(np.fft.rfft(mono[idx] * win[None, :], axis=1))

    diff = np.diff(mag, axis=0)
    flux = np.sum(np.maximum(diff, 0.0), axis=1)
    return np.concatenate([[0.0], flux]), HOP


def pick_onsets(flux, hop, sr, spacing_s):
    """Peak-pick the flux curve into onset sample positions.

    Two guards against over-segmentation, which is the failure mode that
    matters: an extra cut splits one step into two half-steps, and both
    variants then sound broken.
      - adaptive threshold: local median + k * local MAD, so a quiet passage
        is not judged against a loud one's peaks;
      - refractory window at 55% of nominal spacing, which is comfortably
        wider than the heel-to-toe gap within one step but narrower than the
        gap between two steps.
    """
    if len(flux) == 0:
        return []

    # Smooth lightly: single-frame flux is spiky enough to produce twin peaks
    # on one transient.
    k = np.array([0.25, 0.5, 0.25])
    sm = np.convolve(flux, k, mode="same")

    # Local statistics over ~1 s, an order above a step and below the take.
    w = max(3, int(round(sr / hop)))
    if w % 2 == 0:
        w += 1
    pad = w // 2
    padded = np.pad(sm, pad, mode="edge")
    strided = np.lib.stride_tricks.sliding_window_view(padded, w)
    med = np.median(strided, axis=1)
    mad = np.median(np.abs(strided - med[:, None]), axis=1)
    thresh = med + 3.0 * (mad + 1e-9) + 0.05 * sm.max()

    refractory = max(1, int(round(spacing_s * 0.55 * sr / hop)))

    onsets = []
    i = 1
    while i < len(sm) - 1:
        if sm[i] > thresh[i] and sm[i] >= sm[i - 1] and sm[i] >= sm[i + 1]:
            # Take the local maximum across the refractory window, not the
            # first frame over threshold -- the true attack frame is the
            # loudest one, and starting the back-off search from it is more
            # reliable than starting from the shoulder.
            j = min(len(sm), i + refractory)
            best = i + int(np.argmax(sm[i:j]))
            onsets.append(best * hop)
            i = best + refractory
        else:
            i += 1
    return onsets


def backoff_to_floor(mono, sr, onset, prev_end):
    """Walk back from `onset` to the last sample before the attack begins.

    The detected onset is the loudest FRAME, so it sits a few ms INSIDE the
    attack. Cutting there would clip it. We measure a noise floor from the gap
    since the previous slice, then walk backwards while the local energy is
    above that floor -- landing at the foot of the transient -- and finally
    subtract PRE_ROLL_S so the file opens on true silence.
    """
    win = max(1, int(sr * 0.001))
    lo = max(prev_end, onset - int(sr * 0.30))
    if onset - lo < win * 2:
        return max(prev_end, onset - int(sr * PRE_ROLL_S))

    seg = mono[lo:onset]
    # Envelope of the run-up, 1 ms RMS.
    e = np.sqrt(np.convolve(seg ** 2, np.ones(win) / win, mode="same"))
    # Floor = 20th percentile of the run-up: robust to the attack occupying
    # the tail end of this window.
    floor = np.percentile(e, 20) if len(e) else 0.0
    gate = max(floor * 2.5, e.max() * 0.06 if len(e) else 0.0)

    i = len(e) - 1
    while i > 0 and e[i] > gate:
        i -= 1
    cut = lo + i - int(sr * PRE_ROLL_S)
    return int(max(prev_end, min(cut, onset)))


def slice_end(mono, sr, start, next_start):
    """Where this step's tail has decayed enough to cut, bounded by the next."""
    hard = int(min(next_start, start + MAX_LEN_S * sr, len(mono)))
    seg = mono[start:hard]
    if len(seg) < 16:
        return hard

    win = max(1, int(sr * 0.005))
    e = np.sqrt(np.convolve(seg ** 2, np.ones(win) / win, mode="same"))
    peak = e.max()
    if peak <= 0:
        return hard
    floor = peak * (10.0 ** (TAIL_FLOOR_DB / 20.0))

    # Scan back from the end for the last sample still above the tail floor:
    # scanning forward would stop in a momentary dip between the heel and toe
    # halves of one step and truncate the step's own body.
    above = np.nonzero(e > floor)[0]
    if len(above) == 0:
        return hard
    end = start + int(above[-1]) + int(sr * FADE_OUT_S)
    end = max(start + int(MIN_LEN_S * sr), end)
    return int(min(end, hard))


def apply_fades(x, sr):
    n = len(x)
    fi = min(int(sr * FADE_IN_S), n // 2)
    fo = min(int(sr * FADE_OUT_S), n // 2)
    if fi > 0:
        x[:fi] *= np.linspace(0.0, 1.0, fi)
    if fo > 0:
        # Equal-power (cosine) out-fade: on a noise-like tail a linear ramp
        # audibly ducks through the middle of the fade.
        x[n - fo:] *= np.cos(np.linspace(0.0, math.pi / 2.0, fo))
    return x


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def split_file(src_path, set_name, dry_run):
    data, sr = read_wav(src_path)
    mono = data.mean(axis=1)
    # Guard against a source that is already near clipping: analysis uses a
    # normalized copy so the thresholds are level-independent, but the SLICES
    # are cut from the original so the take's dynamics survive.
    peak = float(np.abs(mono).max()) or 1.0
    analysis = mono / peak

    flux, hop = spectral_flux(analysis, sr)
    onsets = pick_onsets(flux, hop, sr, NOMINAL_SPACING)
    if not onsets:
        print("  !! no onsets detected in %s" % os.path.basename(src_path))
        return 0

    slices = []
    prev_end = 0
    for i, on in enumerate(onsets):
        start = backoff_to_floor(analysis, sr, on, prev_end)
        nxt = onsets[i + 1] if i + 1 < len(onsets) else len(mono)
        # The next slice may not start before the next onset's own back-off;
        # approximate it by the midpoint so tails are not stolen.
        limit = nxt if i + 1 >= len(onsets) else (start + nxt) // 2 + int(sr * 0.20)
        end = slice_end(analysis, sr, start, min(limit, nxt))
        if end - start < MIN_LEN_S * sr:
            continue
        slices.append((start, end))
        prev_end = start  # allow tails to overlap the next step's pre-roll

    # Peak-match toward PEAK_TARGET, blended by LOUDNESS_MATCH.
    cut_peaks = [float(np.abs(mono[a:b]).max()) or 1e-9 for a, b in slices]
    out_dir = os.path.join(OUT_ROOT, set_name)
    if not dry_run:
        os.makedirs(out_dir, exist_ok=True)

    written = 0
    for i, ((a, b), p) in enumerate(zip(slices, cut_peaks)):
        gain = (PEAK_TARGET / p) ** LOUDNESS_MATCH
        x = apply_fades(mono[a:b].copy() * gain, sr)
        # Never let the matching push a slice into clipping.
        m = float(np.abs(x).max())
        if m > 0.999:
            x *= 0.999 / m
        name = "%s_%02d.wav" % (set_name, i + 1)
        if dry_run:
            print("     %-16s %7.3f -> %7.3f  (%5.0f ms, peak %.3f -> %.3f)"
                  % (name, a / sr, b / sr, (b - a) * 1000.0 / sr, p, float(np.abs(x).max())))
        else:
            write_wav_mono16(os.path.join(out_dir, name), x, sr)
        written += 1

    print("  %-8s %2d steps  ->  assets/sounds/footsteps/%s/" % (set_name, written, set_name))
    return written


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true",
                    help="print the cuts that would be made, write nothing")
    ap.add_argument("--only", metavar="SET",
                    help="cut only this output set (path/leaf/metal/branch)")
    args = ap.parse_args()

    total = 0
    for src, name in sorted(MAPPING.items(), key=lambda kv: kv[1]):
        if args.only and args.only != name:
            continue
        path = os.path.join(RAW_DIR, src)
        if not os.path.isfile(path):
            print("  !! missing source: %s" % path)
            continue
        total += split_file(path, name, args.dry_run)

    print("%s %d step files" % ("would write" if args.dry_run else "wrote", total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
