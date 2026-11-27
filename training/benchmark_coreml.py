"""Measure Core ML inference latency on this Mac (the M3 acceptance test).

    xcrun coremlcompiler compile training/export/ResonanceFeedback.mlpackage training/export
    python3 training/benchmark_coreml.py --model training/export/ResonanceFeedback.mlmodelc

Prints p50/p95 for CPU only, CPU + Neural Engine and all compute units. The
development target is p95 < 10 ms with batch size 1 and FP16 weights.
Nothing is published as measured until this actually ran on the target Mac.
"""

import argparse
import sys
import time

import numpy as np


def percentile(values, q):
    return float(np.percentile(np.asarray(values), q))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True, help="path to ResonanceFeedback.mlmodelc")
    parser.add_argument("--runs", type=int, default=100)
    parser.add_argument("--warmup", type=int, default=10)
    args = parser.parse_args()

    try:
        import coremltools as ct
    except ImportError:
        sys.exit("coremltools is not installed: pip install -r training/requirements.txt")

    patch = np.random.default_rng(0).random((1, 1, 64, 256), dtype=np.float32)

    configurations = [
        ("CPU only", ct.ComputeUnit.CPU_ONLY),
        ("CPU + Neural Engine", ct.ComputeUnit.CPU_AND_NE),
        ("All units", ct.ComputeUnit.ALL),
    ]

    print(f"model: {args.model}")
    print(f"runs per configuration: {args.runs} (+{args.warmup} warm-up)\n")
    print(f"{'configuration':<22} {'p50':>8} {'p95':>8} {'min':>8} {'max':>8}")

    for name, units in configurations:
        model = ct.models.MLModel(args.model, compute_units=units)

        for _ in range(args.warmup):
            model.predict({"spectrogram": patch})

        times = []

        for _ in range(args.runs):
            started = time.perf_counter()
            prediction = model.predict({"spectrogram": patch})
            times.append((time.perf_counter() - started) * 1000.0)

        mask = np.asarray(prediction["mask"])
        assert mask.shape[-1] == 64, f"unexpected mask shape {mask.shape}"

        print(f"{name:<22} {percentile(times, 50):7.2f}ms {percentile(times, 95):7.2f}ms "
              f"{min(times):7.2f}ms {max(times):7.2f}ms")

    print("\nTarget: p95 < 10 ms. Report the Mac model, macOS version and Xcode version with these numbers.")


if __name__ == "__main__":
    main()
