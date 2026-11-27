"""Synthetic dataset for ResonanceAI (numpy only, no audio files required).

Produces patches that match Source/dsp/DeFeedbackEngine exactly:

    48 kHz, 2048 pt Hann FFT, hop 256
    64 mel bands from 100 Hz to 16 kHz, band value = max dB of its FFT bins
    dB clamped to [-100, 0], normalised to [0, 1] as (dB + 100) / 100
    patch shape [64 bands, 256 frames], band-major, newest frame last

The simulator is not a replacement for real recordings: it teaches the model
the spectral/temporal signature of a building resonance (narrow, growing,
stable in frequency) versus harmonic programme material. Fine-tune on real
multi-track material before shipping.

Usage:
    python3 training/synthesize.py --patches 4000 --out training/dataset/train.npz
"""

import argparse
import os

import numpy as np

SR = 48000
FFT_SIZE = 2048
HOP = 256
BANDS = 64
FRAMES = 256
F_LOW = 100.0
F_HIGH = 16000.0
DB_FLOOR = -100.0


def mel(f):
    return 2595.0 * np.log10(1.0 + np.asarray(f, dtype=np.float64) / 700.0)


def imel(m):
    return 700.0 * (10.0 ** (np.asarray(m, dtype=np.float64) / 2595.0) - 1.0)


def band_geometry(n_bins=FFT_SIZE // 2 + 1):
    edges = imel(np.linspace(mel(F_LOW), mel(F_HIGH), BANDS + 1))
    bin_hz = SR / FFT_SIZE
    lo = np.clip(np.floor(edges[:-1] / bin_hz).astype(int), 1, n_bins - 2)
    hi = np.clip(np.ceil(edges[1:] / bin_hz).astype(int), lo, n_bins - 2)
    centres = 0.5 * (edges[:-1] + edges[1:])
    return lo, hi, centres


BAND_LO, BAND_HI, BAND_CENTRES = band_geometry()


def band_index(freq):
    return int(np.argmin(np.abs(np.log(BAND_CENTRES / max(freq, 1.0)))))


def log_band_spectrogram(x):
    """x: mono float array -> [frames, 64] in normalised dB (0..1)."""
    window = np.hanning(FFT_SIZE)
    if len(x) < FFT_SIZE:
        x = np.pad(x, (0, FFT_SIZE - len(x)))

    count = 1 + (len(x) - FFT_SIZE) // HOP
    frames = np.lib.stride_tricks.sliding_window_view(x, FFT_SIZE)[::HOP][:count]
    spectrum = np.abs(np.fft.rfft(frames * window, axis=1)) * (4.0 / FFT_SIZE)
    db = 20.0 * np.log10(spectrum + 1e-9)

    out = np.empty((count, BANDS), dtype=np.float32)
    for b in range(BANDS):
        out[:, b] = np.max(db[:, BAND_LO[b]:BAND_HI[b] + 1], axis=1)

    out = np.clip(out, DB_FLOOR, 0.0)
    return (out - DB_FLOOR) / -DB_FLOOR


def music_bed(n, rng, level=0.05):
    t = np.arange(n, dtype=np.float64) / SR
    chords = np.array([[110.00, 164.81, 220.00],
                       [87.31, 130.81, 174.61],
                       [130.81, 196.00, 261.63],
                       [98.00, 146.83, 196.00]])
    bass = np.array([55.00, 43.65, 65.41, 49.00])

    out = np.zeros(n)

    bar_len = 2.0
    for bar in range(int(np.ceil(n / SR / bar_len))):
        start = int(bar * bar_len * SR)
        end = min(n, start + int(bar_len * SR))
        if start >= n:
            break

        local = t[start:end] - t[start]
        env = np.minimum(1.0, np.minimum(local / 0.18, (bar_len - local) / 0.35))
        env = np.clip(env, 0.0, 1.0)

        idx = bar % 4
        for v, f0 in enumerate(chords[idx]):
            vib = 1.0 + 0.0012 * np.sin(2.0 * np.pi * 4.3 * t[start:end] + v)
            for h in range(1, 7):
                out[start:end] += (level / h) * env * np.sin(
                    2.0 * np.pi * f0 * h * vib * t[start:end] + 0.8 * h)

        out[start:end] += 0.08 * env * np.sin(2.0 * np.pi * bass[idx] * t[start:end])

    if rng.random() < 0.7:
        for start in range(0, n, int(SR * 0.5)):
            end = min(n, start + int(SR * 0.06))
            env = np.exp(-np.arange(end - start) / (SR * 0.012))
            out[start:end] += 0.16 * env * rng.standard_normal(end - start)

    return out


def add_feedback(x, rng, count=1):
    """Adds exponentially growing resonances; returns (signal, [(band, start, end)])."""
    n = len(x)
    labels = []

    for _ in range(count):
        freq = float(np.exp(rng.uniform(np.log(250.0), np.log(6000.0))))
        start = int(rng.uniform(0.0, max(0.1, n / SR - 1.5)) * SR)
        grow = float(rng.uniform(0.8, 3.0))
        peak = float(rng.uniform(0.05, 0.35))

        idx = np.arange(start, n)
        k = np.clip((idx - start) / (grow * SR), 0.0, 1.0)
        amp = 1e-4 * (peak / 1e-4) ** k
        phase = 2.0 * np.pi * freq * idx / SR

        x[start:] += amp * np.sin(phase)

        # a slightly detuned second component makes the resonance less synthetic
        x[start:] += 0.25 * amp * np.sin(phase * 1.0007 + 0.5)

        labels.append((band_index(freq), start, n))

    return x, labels


def build(patch_count, seed):
    rng = np.random.default_rng(seed)
    lo, hi, _ = band_geometry()

    xs, ys, cs = [], [], []

    while len(xs) < patch_count:
        duration = 9.0
        n = int(duration * SR)
        level = float(rng.uniform(0.03, 0.09))

        signal = music_bed(n, rng, level=level)
        signal += rng.uniform(0.002, 0.01) * rng.standard_normal(n)

        is_positive = rng.random() < 0.5
        labels = []

        if is_positive:
            signal, labels = add_feedback(signal, rng, count=int(rng.integers(1, 3)))

        signal = signal * rng.uniform(0.7, 1.4)

        patch = log_band_spectrogram(signal)
        last_frame = patch.shape[0] - 1

        # random crop that still contains the newest 256 frames of interest
        offset = int(rng.integers(0, max(1, patch.shape[0] - FRAMES - 1)))
        window = patch[offset:offset + FRAMES]

        if window.shape[0] < FRAMES:
            continue

        mask = np.zeros(BANDS, dtype=np.float32)
        end_frame = offset + FRAMES - 1
        end_sample = end_frame * HOP + FFT_SIZE

        for band, start, _ in labels:
            if end_sample >= start:
                mask[max(0, band - 1):min(BANDS, band + 2)] = 1.0

        if is_positive and mask.sum() == 0:
            continue

        xs.append(window.T.astype(np.float16))  # [bands, frames]
        ys.append(mask.astype(np.float16))
        cs.append(np.float16(1.0 if mask.sum() > 0 else 0.0))

    return (np.stack(xs), np.stack(ys), np.stack(cs))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--patches", type=int, default=4000)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--out", default=os.path.join("training", "dataset", "train.npz"))
    args = parser.parse_args()

    os.makedirs(os.path.dirname(args.out), exist_ok=True)

    x, y, c = build(args.patches, args.seed)
    np.savez_compressed(args.out, x=x, y=y, c=c)

    positives = int((c > 0).sum())
    print(f"wrote {args.out}: x={x.shape} y={y.shape} positives={positives}/{len(c)}")


if __name__ == "__main__":
    main()
