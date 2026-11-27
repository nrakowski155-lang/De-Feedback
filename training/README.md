# ResonanceAI – Training Pipeline

The plugin ships the **inference contract and the fallback**, not a trained
model. This directory contains the pipeline that produces the model:

```
synthesize.py  →  train.py  →  export_coreml.py  →  benchmark_coreml.py
   patches        CNN + GRU        .mlpackage          CPU / NE / All
```

> **Status:** the scripts are written against the exact tensor contract of the
> plugin, but they were **not executed in this workspace** (no PyTorch, no
> Core ML, no M3). The numbers in the table below are development targets, not
> measured results. Run the pipeline on the target Mac and record the real
> values before quoting them.

## 1. Contract (fixed – must match `Source/ai/AiTypes.h`)

| Property | Value |
|---|---|
| Sample rate | 48 kHz |
| Analysis FFT | 2048 pt Hann, hop 256 → 5.33 ms |
| Confirmation FFT | 4096 pt, every 16 hops (~85 ms) |
| Bands | 64 mel bands, 100 Hz – 16 kHz |
| Band value | max dB of the band's FFT bins, clamped −100…0 dBFS |
| Normalisation | `(dB + 100) / 100`, i.e. 0…1 |
| Patch | `[64 bands, 256 frames]`, band-major, newest frame last → 1.37 s |
| Input tensor | `"spectrogram"`, float32 `[1, 1, 64, 256]` |
| Output `"mask"` | float32 `[1, 64]`, feedback probability per band |
| Output `"clip"` | float32 `[1, 1]`, feedback probability for the patch |
| Precision | FP16 weights, batch 1 |
| Model size | ~2.4 MB weights, target < 10 MB |

The engine only accepts a mask that is younger than 500 ms. A mask can add up
to **+8 dB** of prominence and, above 0.70 confidence, overrule the music guard.
It can never veto a DSP detection – an unvalidated model must not be able to
suppress a real howl.

## 2. Commands

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r training/requirements.txt

# ~4000 patches, 9 s of synthetic programme material each
python3 training/synthesize.py --patches 4000

# ~1.21 M parameters, MPS is used automatically on Apple Silicon
python3 training/train.py --epochs 30

# .mlpackage (FP16, macOS 12+), then compile it for the plugin
python3 training/export_coreml.py --compile

# latency of the three Core ML configurations
python3 training/benchmark_coreml.py \
    --model training/export/ResonanceFeedback.mlmodelc
```

`training/dataset/`, `training/checkpoints/` and `training/export/` are
generated and git-ignored.

## 3. Installing the model into the plugin

The processor looks for `ResonanceFeedback.mlmodelc` in this order:

1. `$RESONANCE_AI_MODEL` (full path)
2. `~/Library/Application Support/ResonanceAI/ResonanceFeedback.mlmodelc`
3. inside the plugin bundle (`Contents/Resources/`)

The simplest install:

```bash
mkdir -p ~/Library/Application\ Support/ResonanceAI
cp -R training/export/ResonanceFeedback.mlmodelc \
      ~/Library/Application\ Support/ResonanceAI/
```

Then restart Logic (or the host) so the plugin reloads the model. The editor
status line shows `CORE ML` once the model is active and `BUILTIN` while the
fallback is in use.

Compute units (benchmark configuration) can be forced with:

```bash
RESONANCE_AI_COMPUTE_UNITS=1   # CPU only
RESONANCE_AI_COMPUTE_UNITS=2   # CPU + Neural Engine (default)
RESONANCE_AI_COMPUTE_UNITS=3   # all available units
```

`MLModelConfiguration.computeUnits` requires macOS 12; the bridge falls back to
the default configuration on older systems.

## 4. Data

`synthesize.py` renders:

* four-chord pad with six harmonics per voice, bass and drums (programme material)
* one or two exponentially growing resonances (250 Hz – 6 kHz), optionally detuned
* a noise floor and random gain, so the model sees different programme levels

Labels are per-band and only mark the bands of the active resonances (±1 band).
This is enough to learn the *narrow + growing + stable* signature, but it is
**not** a substitute for real recordings. Before shipping:

1. record real loop feedback through the actual microphone/monitor path,
2. record sustained vocals, choir, piano, guitar and cymbals as hard negatives,
3. fine-tune the synthetic checkpoint on that material,
4. keep a held-out set per instrument and per room.

The DSP tests in `tests/dsp_test.cpp` already cover the "no false positives on
programme material" case; the model must not make that worse.

## 5. Targets vs. reality

| Metric | Target | Measured |
|---|---|---|
| Parameters | ~1.2 M (1.21 M by construction) | `python3 training/model.py` prints the exact count |
| Model size | < 10 MB | ~2.4 MB FP16 weights |
| Inference p95 | < 10 ms | **not measured** – run `benchmark_coreml.py` on the M3 |
| Memory | < 150 MB runtime | **not measured** |
| Detection latency | < 100 ms where acoustically possible | DSP: 2048 pt window + attack (default 250 ms, min 10 ms); AI adds one patch interval (~85 ms) |
| Added DSP latency | 0 samples | by design, causal filters, no lookahead |

The output of `benchmark_coreml.py` on the target machine belongs in the
project README and in the release notes – with hardware, macOS and Xcode
version.
