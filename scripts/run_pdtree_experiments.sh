#!/usr/bin/env bash
set -euo pipefail
CONFIGS=(
00_baseline_rc_only.json
01_proposed_default.json
02_ablation_no_hbt_net_penalty.json
03_hbt_conservative.json
04_timing_aggressive.json
05_high_hbt_rc.json
06_low_hbt_rc.json
07_wire_delay_dominant.json
08_parent_load_dominant.json
09_no_stretch_penalty.json
10_high_stretch_penalty.json
11_high_sink_cap.json
12_low_sink_cap.json
13_top_rc_high.json
14_bottom_rc_high.json
15_traditional_pdtree.json
16_proposed_with_2d_plots.json
)

BIN=${1:-./build/src/test_myrouter_PDtree/test_myrouter_PDtree}
for c in "${CONFIGS[@]}"; do
  cfg="configs/${c}"
  echo "[run] ${cfg}"
  "${BIN}" --config "${cfg}"
  out_dir=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["output"]["output_dir"])' "${cfg}")
  python3 scripts/plot_nets.py --root "${out_dir}"
done
