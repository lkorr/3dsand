#!/usr/bin/env python3
"""Split concatenated sound files into individual mono 16-bit WAV samples.

Each input file is a sequence of sounds separated by silence. The script
detects the gaps and writes each segment as a separate file, converting
stereo to mono and 24-bit to 16-bit to match the engine's existing format.
"""

import os
import sys
import wave
import struct
import numpy as np

SOUNDS_DIR = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'assets', 'sounds')

# Silence detection: RMS below this threshold (relative to 16-bit peak)
SILENCE_THRESH = 0.008
# Minimum silence gap in seconds to split on
MIN_GAP_SEC = 0.15
# Minimum sample duration in seconds (discard shorter fragments)
MIN_SAMPLE_SEC = 0.08
# Pad: keep this many seconds of lead-in/lead-out around each sample
PAD_SEC = 0.02

def read_wav_as_float(path):
    """Read a WAV file and return (samples_float_mono, sample_rate)."""
    w = wave.open(path, 'rb')
    nch = w.getnchannels()
    sw = w.getsampwidth()
    rate = w.getframerate()
    nframes = w.getnframes()
    raw = w.readframes(nframes)
    w.close()

    if sw == 2:
        data = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    elif sw == 3:
        # 24-bit: unpack manually
        n = len(raw) // 3
        arr = np.zeros(n, dtype=np.int32)
        for i in range(n):
            b = raw[i*3:(i+1)*3]
            val = int.from_bytes(b, 'little', signed=False)
            if val >= 0x800000:
                val -= 0x1000000
            arr[i] = val
        data = arr.astype(np.float32) / 8388608.0
    elif sw == 4:
        data = np.frombuffer(raw, dtype=np.int32).astype(np.float32) / 2147483648.0
    else:
        raise ValueError(f'Unsupported sample width {sw}')

    if nch > 1:
        data = data.reshape(-1, nch).mean(axis=1)

    return data, rate


def find_segments(data, rate):
    """Find non-silent segments. Returns list of (start_sample, end_sample)."""
    window = int(rate * 0.01)  # 10ms RMS window
    pad = int(rate * PAD_SEC)
    min_gap = int(rate * MIN_GAP_SEC)
    min_len = int(rate * MIN_SAMPLE_SEC)

    # Compute RMS in windows
    n_windows = len(data) // window
    rms = np.zeros(n_windows)
    for i in range(n_windows):
        chunk = data[i*window:(i+1)*window]
        rms[i] = np.sqrt(np.mean(chunk**2))

    # Find runs of non-silent windows
    active = rms > SILENCE_THRESH
    segments = []
    in_seg = False
    seg_start = 0
    silent_run = 0

    for i, a in enumerate(active):
        if a:
            if not in_seg:
                seg_start = i
                in_seg = True
            silent_run = 0
        else:
            if in_seg:
                silent_run += 1
                if silent_run * window >= min_gap:
                    seg_end = i - silent_run
                    start_samp = max(0, seg_start * window - pad)
                    end_samp = min(len(data), (seg_end + 1) * window + pad)
                    if end_samp - start_samp >= min_len:
                        segments.append((start_samp, end_samp))
                    in_seg = False
                    silent_run = 0

    if in_seg:
        start_samp = max(0, seg_start * window - pad)
        end_samp = len(data)
        if end_samp - start_samp >= min_len:
            segments.append((start_samp, end_samp))

    return segments


def write_wav_16bit_mono(path, data_float, rate):
    """Write a mono 16-bit WAV file from float data."""
    clipped = np.clip(data_float, -1.0, 1.0)
    int16 = (clipped * 32767).astype(np.int16)
    w = wave.open(path, 'wb')
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(rate)
    w.writeframes(int16.tobytes())
    w.close()


def split_file(src_path, dest_dir, prefix):
    """Split one concatenated WAV into individual samples."""
    print(f'\n--- {os.path.basename(src_path)} ---')
    data, rate = read_wav_as_float(src_path)
    print(f'  {len(data)/rate:.1f}s, {rate} Hz')

    segments = find_segments(data, rate)
    print(f'  found {len(segments)} segments')

    os.makedirs(dest_dir, exist_ok=True)
    for i, (s, e) in enumerate(segments):
        fname = f'{prefix}_{i+1:02d}.wav'
        fpath = os.path.join(dest_dir, fname)
        write_wav_16bit_mono(fpath, data[s:e], rate)
        dur = (e - s) / rate
        print(f'  {fname}: {dur:.3f}s')

    return len(segments)


def main():
    files = [
        ('wind swooshes.wav',                    'melee/whoosh',        'whoosh'),
        ('sword strikes 1.wav',                  'melee/strike_edge',   'strike_edge'),
        ('mace thumps.wav',                      'melee/strike_blunt',  'strike_blunt'),
        ('flesh hit thump.wav',                  'melee/flesh',         'flesh'),
        ('flesh hit thumps 2.wav',               'melee/flesh',         'flesh2'),
        ('dismember and sword cuts on flesh.wav', 'melee/cut',          'cut'),
    ]

    total = 0
    for src_name, dest_rel, prefix in files:
        src = os.path.join(SOUNDS_DIR, src_name)
        if not os.path.exists(src):
            print(f'SKIP: {src_name} not found')
            continue
        dest = os.path.join(SOUNDS_DIR, dest_rel)
        n = split_file(src, dest, prefix)
        total += n

    print(f'\n=== {total} samples written ===')


if __name__ == '__main__':
    main()
