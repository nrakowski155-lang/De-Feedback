"""Train ResonanceAI Balanced v1 on the synthetic patch dataset.

    python3 training/synthesize.py --patches 4000
    python3 training/train.py --epochs 30

The checkpoint stores the state dict plus the contract constants, so the export
script cannot silently drift from Source/ai/AiTypes.h.
"""

import argparse
import os
import time

import numpy as np
import torch
import torch.nn.functional as F
from torch.utils.data import DataLoader, TensorDataset

from model import ResonanceNet, count_parameters

CONTRACT = {"bands": 64, "frames": 256, "normalisation": "(dB + 100) / 100"}


def load_dataset(path):
    data = np.load(path)
    x = torch.from_numpy(data["x"].astype(np.float32)).unsqueeze(1)  # [N, 1, 64, 256]
    y = torch.from_numpy(data["y"].astype(np.float32))               # [N, 64]
    c = torch.from_numpy(data["c"].astype(np.float32)).unsqueeze(1)  # [N, 1]
    return TensorDataset(x, y, c)


def band_scores(pred, target, threshold=0.5):
    active = pred >= threshold
    truth = target >= 0.5
    tp = (active & truth).sum().item()
    fp = (active & ~truth).sum().item()
    fn = (~active & truth).sum().item()
    precision = tp / max(1, tp + fp)
    recall = tp / max(1, tp + fn)
    return precision, recall


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", default=os.path.join("training", "dataset", "train.npz"))
    parser.add_argument("--out", default=os.path.join("training", "checkpoints", "resonance_net.pt"))
    parser.add_argument("--epochs", type=int, default=30)
    parser.add_argument("--batch", type=int, default=16)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    torch.manual_seed(args.seed)
    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")

    dataset = load_dataset(args.data)
    val_size = max(1, len(dataset) // 10)
    train_size = len(dataset) - val_size
    train_set, val_set = torch.utils.data.random_split(
        dataset, [train_size, val_size], generator=torch.Generator().manual_seed(args.seed))

    train_loader = DataLoader(train_set, batch_size=args.batch, shuffle=True, drop_last=False)
    val_loader = DataLoader(val_set, batch_size=args.batch, shuffle=False)

    model = ResonanceNet().to(device)
    print(f"device={device.type}  parameters={count_parameters(model):,}")

    optimiser = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimiser, T_max=args.epochs)

    best = float("inf")
    os.makedirs(os.path.dirname(args.out), exist_ok=True)

    for epoch in range(1, args.epochs + 1):
        model.train()
        total = 0.0
        started = time.time()

        for x, y, c in train_loader:
            x, y, c = x.to(device), y.to(device), c.to(device)
            optimiser.zero_grad(set_to_none=True)

            mask, clip = model(x)
            loss = F.binary_cross_entropy(mask, y, weight=1.0 + 3.0 * y)
            loss = loss + 0.5 * F.binary_cross_entropy(clip, c)

            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimiser.step()
            total += loss.item() * x.size(0)

        scheduler.step()

        model.eval()
        val_loss = 0.0
        p_sum = r_sum = 0.0
        batches = 0

        with torch.no_grad():
            for x, y, c in val_loader:
                x, y, c = x.to(device), y.to(device), c.to(device)
                mask, clip = model(x)
                val_loss += (F.binary_cross_entropy(mask, y, weight=1.0 + 3.0 * y)
                             + 0.5 * F.binary_cross_entropy(clip, c)).item() * x.size(0)
                p, r = band_scores(mask, y)
                p_sum += p
                r_sum += r
                batches += 1

        val_loss /= max(1, len(val_set))
        precision = p_sum / max(1, batches)
        recall = r_sum / max(1, batches)
        print(f"epoch {epoch:02d}  train={total / max(1, train_size):.4f}  "
              f"val={val_loss:.4f}  P={precision:.3f}  R={recall:.3f}  "
              f"{time.time() - started:.1f}s")

        if val_loss < best:
            best = val_loss
            torch.save({"model": model.state_dict(), "contract": CONTRACT, "epoch": epoch},
                       args.out)
            print(f"  saved {args.out} (val={best:.4f})")


if __name__ == "__main__":
    main()
