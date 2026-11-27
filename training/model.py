"""ResonanceAI Balanced v1 - CNN + GRU band-mask model.

Contract (must match Source/ai/AiTypes.h and Source/ai/CoreMlMaskModel.mm):

    input   "spectrogram"  float32 [1, 1, 64, 256]   band-major, newest frame last
    output  "mask"         float32 [1, 64]           feedback probability per band
    output  "clip"         float32 [1, 1]            feedback probability per patch

Values are normalised dB: v = clamp((dB + 100) / 100, 0, 1).

The model is deliberately small (~1.21 M parameters, ~2.4 MB in FP16) so it can
run on the M3 Neural Engine with batch size 1 and well under 10 ms p95.

Run this file directly to print the parameter count:
    python3 training/model.py
"""

import torch
import torch.nn as nn


class ResonanceNet(nn.Module):
    def __init__(self, bands: int = 64, frames: int = 256, hidden: int = 256):
        super().__init__()
        self.bands = bands
        self.frames = frames

        self.features = nn.Sequential(
            nn.Conv2d(1, 24, kernel_size=3, padding=1),
            nn.BatchNorm2d(24),
            nn.ReLU(inplace=True),
            nn.Conv2d(24, 48, kernel_size=3, padding=1),
            nn.BatchNorm2d(48),
            nn.ReLU(inplace=True),
            nn.MaxPool2d((2, 1)),          # 64 -> 32 bands
            nn.Conv2d(48, 96, kernel_size=3, padding=1),
            nn.BatchNorm2d(96),
            nn.ReLU(inplace=True),
            nn.MaxPool2d((2, 1)),          # 32 -> 16 bands
            nn.Conv2d(96, 256, kernel_size=3, padding=1),
            nn.BatchNorm2d(256),
            nn.ReLU(inplace=True),
            nn.MaxPool2d((2, 1)),          # 16 -> 8 bands
        )

        self.project = nn.Linear(256 * (bands // 8), hidden)
        self.gru = nn.GRU(hidden, hidden, batch_first=True)
        self.mask_head = nn.Linear(hidden, bands)
        self.clip_head = nn.Linear(hidden, 1)

    def forward(self, x: torch.Tensor):
        # x: [B, 1, bands, frames]
        h = self.features(x)                       # [B, 256, bands/8, frames]
        b, c, f, t = h.shape
        h = h.permute(0, 3, 1, 2).reshape(b, t, c * f)  # [B, frames, 2048]
        h = torch.relu(self.project(h))
        h, _ = self.gru(h)                         # [B, frames, hidden]
        last = h[:, -1]
        return torch.sigmoid(self.mask_head(last)), torch.sigmoid(self.clip_head(last))


def count_parameters(model: nn.Module) -> int:
    return sum(p.numel() for p in model.parameters() if p.requires_grad)


if __name__ == "__main__":
    net = ResonanceNet()
    total = count_parameters(net)
    print(f"ResonanceNet Balanced v1: {total:,} trainable parameters")
    print(f"FP16 weight size: {total * 2 / 1e6:.2f} MB (target < 10 MB)")

    assert 1_100_000 < total < 1_350_000, "parameter budget drifted from the ~1.2 M target"

    dummy = torch.zeros(1, 1, 64, 256)
    mask, clip = net(dummy)
    assert mask.shape == (1, 64) and clip.shape == (1, 1)
    print("forward shapes OK:", tuple(mask.shape), tuple(clip.shape))
