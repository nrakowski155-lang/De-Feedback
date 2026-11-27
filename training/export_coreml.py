"""Export a trained ResonanceAI checkpoint to a Core ML .mlpackage.

    python3 training/export_coreml.py --checkpoint training/checkpoints/resonance_net.pt
    xcrun coremlcompiler compile training/export/ResonanceFeedback.mlpackage training/export

The compiled bundle is `ResonanceFeedback.mlmodelc`; copy it to
~/Library/Application Support/ResonanceAI/ (or point $RESONANCE_AI_MODEL at it).
"""

import argparse
import os
import subprocess
import sys

import numpy as np
import torch

from model import ResonanceNet


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoint", default=os.path.join("training", "checkpoints", "resonance_net.pt"))
    parser.add_argument("--out", default=os.path.join("training", "export", "ResonanceFeedback.mlpackage"))
    parser.add_argument("--compile", action="store_true", help="run xcrun coremlcompiler afterwards")
    args = parser.parse_args()

    try:
        import coremltools as ct
    except ImportError:
        sys.exit("coremltools is not installed: pip install -r training/requirements.txt")

    checkpoint = torch.load(args.checkpoint, map_location="cpu")
    model = ResonanceNet()
    model.load_state_dict(checkpoint["model"])
    model.eval()

    example = torch.zeros(1, 1, 64, 256, dtype=torch.float32)
    traced = torch.jit.trace(model, example)

    mlmodel = ct.convert(
        traced,
        convert_to="mlprogram",
        inputs=[ct.TensorType(name="spectrogram", shape=(1, 1, 64, 256))],
        outputs=[ct.TensorType(name="mask"), ct.TensorType(name="clip")],
        compute_precision=ct.precision.FLOAT16,
        minimum_deployment_target=ct.target.macOS12,
        compute_units=ct.ComputeUnit.CPU_AND_NE,
    )

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    mlmodel.save(args.out)

    spec = mlmodel.get_spec()
    print(f"wrote {args.out}")
    print(f"inputs : {[str(i.name) for i in spec.description.input]}")
    print(f"outputs: {[str(o.name) for o in spec.description.output]}")

    # sanity check on the CPU runtime before shipping
    reference = ResonanceNet()
    reference.load_state_dict(checkpoint["model"])
    reference.eval()

    with torch.no_grad():
        patch = torch.from_numpy(
            np.random.default_rng(0).random((1, 1, 64, 256), dtype=np.float32))
        mask, clip = reference(patch)

    print(f"reference mask[0,:4]={mask[0, :4].numpy().round(3)}  clip={float(clip):.3f}")

    if args.compile:
        out_dir = os.path.dirname(args.out)
        subprocess.run(["xcrun", "coremlcompiler", "compile", args.out, out_dir], check=True)
        print(f"compiled: {os.path.join(out_dir, 'ResonanceFeedback.mlmodelc')}")


if __name__ == "__main__":
    main()
