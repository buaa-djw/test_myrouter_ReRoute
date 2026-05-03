#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import matplotlib.pyplot as plt


def load_jsons(plot_dir: Path):
    return sorted(plot_dir.glob("*.json"))


def draw_net(data, out_path: Path, show_tree_hbt_node: bool):
    fig = plt.figure(figsize=(10, 8))
    ax = fig.add_subplot(111, projection="3d")
    name = data.get("net_name", "unknown")
    valid = data.get("validation", {}).get("valid", True)
    status = data.get("status", "unknown")

    has_invalid_non_hbt = False
    has_hbt_node = False

    for s in data.get("segments", []):
        x = [s["x1"], s["x2"]]
        y = [s["y1"], s["y2"]]
        z = [s["z1"], s["z2"]]
        uses_hbt = s.get("uses_hbt", False)

        if (not uses_hbt) and z[0] != z[1]:
            has_invalid_non_hbt = True
            ax.plot(x, y, z, color="purple", linestyle="--", linewidth=2, label="invalid non-HBT cross-die")
        elif uses_hbt:
            ax.plot(x, y, z, color="green", linewidth=2, label="HBT vertical")
        elif z[0] > 0.5:
            ax.plot(x, y, z, color="blue", linewidth=1, label="top wire")
        else:
            ax.plot(x, y, z, color="cyan", linewidth=1, label="bottom wire")

    for p in data.get("points", []):
        t = p.get("type", "steiner")
        if t == "source":
            ax.scatter(p["x"], p["y"], p["z"], marker="*", s=120, c="gold", label="source")
        elif t == "sink":
            ax.scatter(p["x"], p["y"], p["z"], marker="o", s=30, c="black", label="sink")
        elif t == "hbt":
            ax.scatter(p["x"], p["y"], p["z"], marker="^", s=80, c="green", label="HBT")
        elif t == "hbt_node" and show_tree_hbt_node:
            has_hbt_node = True
            ax.scatter(p["x"], p["y"], p["z"], marker="s", s=40, c="orange", label="hbt_node")
        else:
            ax.scatter(p["x"], p["y"], p["z"], marker="x", s=20, c="gray")

    effective_valid = valid and not has_invalid_non_hbt
    fail_reason = data.get("fail_reason", "")
    title = f"{name}\nvalidation={'OK' if effective_valid else 'INVALID'} status={status}"
    if not effective_valid and fail_reason:
        title += f"\nfail_reason={fail_reason}"
    ax.set_title(title)

    handles, labels = ax.get_legend_handles_labels()
    uniq = dict(zip(labels, handles))
    required = [
        "top wire",
        "bottom wire",
        "HBT vertical",
        "invalid non-HBT cross-die",
        "source",
        "sink",
        "HBT",
    ]
    if has_hbt_node:
        required.append("hbt_node")
    legend_labels = [x for x in required if x in uniq]
    ax.legend([uniq[x] for x in legend_labels], legend_labels, loc="upper left")
    plt.savefig(out_path, bbox_inches="tight")
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--show-tree-hbt-node", action="store_true")
    args = ap.parse_args()

    root = Path(args.root)
    plot_dir = root / "plot_data"
    out_dir = root / "plots"
    out_dir.mkdir(parents=True, exist_ok=True)

    for f in load_jsons(plot_dir):
        data = json.loads(f.read_text())
        suffix = "_INVALID" if not data.get("validation", {}).get("valid", True) else ""
        draw_net(data, out_dir / f"{f.stem}{suffix}.png", args.show_tree_hbt_node)


if __name__ == "__main__":
    main()
