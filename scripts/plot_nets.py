#!/usr/bin/env python3
import argparse
import json
from pathlib import Path

import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d import Axes3D  # noqa: F401


TOP_WIRE_COLOR = "#1f77b4"       # blue
BOTTOM_WIRE_COLOR = "#ff7f0e"    # orange
HBT_COLOR = "#d62728"            # red
INVALID_COLOR = "#9467bd"        # purple
SOURCE_COLOR = "#d4a017"         # golden
SINK_COLOR = "#111111"           # black
HBT_POINT_COLOR = "#2ca02c"      # green
OTHER_POINT_COLOR = "#7f7f7f"    # gray


def load_json_files(plot_dir: Path):
    if not plot_dir.exists():
        return []
    return sorted(plot_dir.glob("*.json"))


def safe_float(value, default=0.0):
    try:
        return float(value)
    except Exception:
        return default


def infer_point_type(point, index, hbt_coord_set):
    """
    Infer point type for legacy JSON that may not contain 'type'.
    Priority:
    1. explicit 'type'
    2. id == 0 or first point -> source
    3. point located on HBT endpoints -> hbt
    4. otherwise -> sink
    """
    point_type = point.get("type")
    if point_type:
        return point_type

    pid = point.get("id", index)
    key = (point.get("x", 0), point.get("y", 0), point.get("z", 0.0))

    if pid == 0 or index == 0:
        return "source"
    if key in hbt_coord_set:
        return "hbt"
    return "sink"


def collect_hbt_coords(data):
    """
    Collect endpoint coordinates from HBT segments,
    so legacy point arrays can still infer HBT markers.
    """
    coords = set()
    for seg in data.get("segments", []):
        if seg.get("uses_hbt", False):
            coords.add((seg["x1"], seg["y1"], seg["z1"]))
            coords.add((seg["x2"], seg["y2"], seg["z2"]))
    return coords


def build_legend_items(ax, data):
    """
    Build deduplicated legend items based on what actually appears.
    """
    handles, labels = ax.get_legend_handles_labels()
    unique = {}
    for handle, label in zip(handles, labels):
        if label not in unique:
            unique[label] = handle

    order = [
        "top wire",
        "bottom wire",
        "HBT vertical",
        "invalid non-HBT cross-die",
        "source",
        "sink",
        "HBT",
        "other point",
    ]

    legend_labels = [label for label in order if label in unique]
    legend_handles = [unique[label] for label in legend_labels]
    return legend_handles, legend_labels


def draw_segments(ax, data):
    for seg in data.get("segments", []):
        x = [seg["x1"], seg["x2"]]
        y = [seg["y1"], seg["y2"]]
        z = [seg["z1"], seg["z2"]]

        uses_hbt = seg.get("uses_hbt", False)
        is_cross_die_non_hbt = (not uses_hbt) and (seg["z1"] != seg["z2"])

        if is_cross_die_non_hbt:
            ax.plot(
                x, y, z,
                color=INVALID_COLOR,
                linestyle="--",
                linewidth=2.4,
                label="invalid non-HBT cross-die"
            )
        elif uses_hbt:
            ax.plot(
                x, y, z,
                color=HBT_COLOR,
                linewidth=2.8,
                label="HBT vertical"
            )
        elif z[0] > 0.5:
            ax.plot(
                x, y, z,
                color=TOP_WIRE_COLOR,
                linewidth=2.0,
                label="top wire"
            )
        else:
            ax.plot(
                x, y, z,
                color=BOTTOM_WIRE_COLOR,
                linewidth=2.0,
                label="bottom wire"
            )


def draw_points(ax, data, label_points=False):
    hbt_coords = collect_hbt_coords(data)

    for idx, point in enumerate(data.get("points", [])):
        point_type = infer_point_type(point, idx, hbt_coords)

        x = point.get("x", 0)
        y = point.get("y", 0)
        z = point.get("z", 0.0)

        if point_type == "source":
            ax.scatter(
                x, y, z,
                marker="*",
                s=150,
                c=SOURCE_COLOR,
                edgecolors="black",
                linewidths=0.8,
                label="source"
            )
        elif point_type == "sink":
            ax.scatter(
                x, y, z,
                marker="o",
                s=28,
                c=SINK_COLOR,
                label="sink"
            )
        elif point_type == "hbt":
            ax.scatter(
                x, y, z,
                marker="^",
                s=85,
                c=HBT_POINT_COLOR,
                edgecolors="black",
                linewidths=0.5,
                label="HBT"
            )
        else:
            ax.scatter(
                x, y, z,
                marker="x",
                s=22,
                c=OTHER_POINT_COLOR,
                label="other point"
            )

        if label_points:
            pid = point.get("id", idx)
            ax.text(x, y, z, f"{pid}", fontsize=8)


def set_axes_style(ax, data):
    ax.set_xlabel("x", fontsize=11, labelpad=8)
    ax.set_ylabel("y", fontsize=11, labelpad=8)
    ax.set_zlabel("z", fontsize=11, labelpad=8)

    ax.grid(True, linestyle=":", linewidth=0.5, alpha=0.7)

    is_3d = bool(data.get("is_3d", False))
    if not is_3d:
        ax.set_zlim(-0.05, 0.05)

    try:
        ax.set_box_aspect((1.2, 1.2, 0.7))
    except Exception:
        pass

    ax.view_init(elev=28, azim=-60)


def add_right_panel(fig, ax, data):
    """
    Right-side panel:
    - legend on top
    - detail box below
    """
    legend_handles, legend_labels = build_legend_items(ax, data)

    if legend_handles:
        fig.legend(
            legend_handles,
            legend_labels,
            loc="upper left",
            bbox_to_anchor=(0.80, 0.88),
            frameon=True,
            fontsize=10
        )

    wl = safe_float(data.get("wirelength", 0.0))
    avg_delay = safe_float(data.get("avg_sink_delay", 0.0))
    max_delay = safe_float(data.get("max_sink_delay", 0.0))

    info_text = (
        f"WL: {wl:.0f}\n"
        f"avg_delay: {avg_delay:.4f}\n"
        f"max_delay: {max_delay:.4f}"
    )

    fig.text(
        0.80,
        0.60,
        info_text,
        ha="left",
        va="top",
        fontsize=11,
        bbox=dict(boxstyle="round,pad=0.5", facecolor="white", alpha=0.92, edgecolor="gray")
    )


def draw_one(json_path: Path, out_dir: Path, dpi: int, label_points: bool):
    data = json.loads(json_path.read_text())

    fig = plt.figure(figsize=(13.5, 9.0))
    ax = fig.add_subplot(111, projection="3d")

    # Leave space at right side for legend + detail box
    fig.subplots_adjust(left=0.06, right=0.76, top=0.90, bottom=0.08)

    draw_segments(ax, data)
    draw_points(ax, data, label_points=label_points)
    set_axes_style(ax, data)

    # Title: only net name
    net_name = data.get("net_name", json_path.stem)
    ax.set_title(net_name, fontsize=18, fontweight="bold", pad=18)

    add_right_panel(fig, ax, data)

    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / f"{json_path.stem}.png"
    fig.savefig(out_path, dpi=dpi, bbox_inches="tight")
    plt.close(fig)


def process_root(root: Path, dpi: int, label_points: bool):
    plot_data_2d = root / "plot_data" / "2d"
    plot_data_3d = root / "plot_data" / "3d"
    plots_2d = root / "plots" / "2d"
    plots_3d = root / "plots" / "3d"

    # New structure: plot_data/2d and plot_data/3d
    files_2d = load_json_files(plot_data_2d)
    files_3d = load_json_files(plot_data_3d)
    if not plot_data_2d.exists():
        print(f"[plot_nets] skip missing directory: {plot_data_2d}")
    if not plot_data_3d.exists():
        print(f"[plot_nets] skip missing directory: {plot_data_3d}")

    if files_2d or files_3d:
        for js in files_2d:
            draw_one(js, plots_2d, dpi=dpi, label_points=label_points)
        for js in files_3d:
            draw_one(js, plots_3d, dpi=dpi, label_points=label_points)
        return

    # Legacy fallback: plot_data/*.json
    legacy_plot_data = root / "plot_data"
    legacy_plots = root / "plots"
    if legacy_plot_data.exists():
        for js in load_json_files(legacy_plot_data):
            draw_one(js, legacy_plots, dpi=dpi, label_points=label_points)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, help="Experiment result root directory")
    parser.add_argument("--dpi", type=int, default=260, help="Output PNG DPI")
    parser.add_argument("--label-points", action="store_true", help="Show point id labels")
    args = parser.parse_args()

    root = Path(args.root)
    process_root(root, dpi=args.dpi, label_points=args.label_points)


if __name__ == "__main__":
    main()
