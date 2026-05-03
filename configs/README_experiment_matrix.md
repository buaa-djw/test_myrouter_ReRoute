# PDTreeRouter JSON experiment set

Copy these files into:

`/home/djw/Desktop/TaiWei/ORFS-Research/tools/OpenROAD/src/test_myrouter_PDtree/configs/`

Then run each file with `--config`.

| File | Experiment | Purpose |
|---|---|---|
| `00_baseline_rc_only.json` | `gcd_00_baseline_rc_only` | RC delay only; HBT-count penalty disabled and HBT limit relaxed. |
| `01_proposed_default.json` | `gcd_01_proposed_default` | Main proposed RC-delay + HBT-aware cost. |
| `02_ablation_no_hbt_net_penalty.json` | `gcd_02_ablation_no_hbt_net_penalty` | Ablation without HBT resource penalty. |
| `03_hbt_conservative.json` | `gcd_03_hbt_conservative` | Strong HBT resource constraint. |
| `04_timing_aggressive.json` | `gcd_04_timing_aggressive` | Weak HBT-count penalty; more HBTs allowed. |
| `05_high_hbt_rc.json` | `gcd_05_high_hbt_rc` | High HBT RC; tests HBT-RC sensitivity. |
| `06_low_hbt_rc.json` | `gcd_06_low_hbt_rc` | Low HBT RC; cheap HBT scenario. |
| `07_wire_delay_dominant.json` | `gcd_07_wire_delay_dominant` | Emphasizes wire RC delay. |
| `08_parent_load_dominant.json` | `gcd_08_parent_load_dominant` | Emphasizes parent-load delay / shallow timing tree. |
| `09_no_stretch_penalty.json` | `gcd_09_no_stretch_penalty` | Ablation without stretch penalty. |
| `10_high_stretch_penalty.json` | `gcd_10_high_stretch_penalty` | Strong stretch penalty. |

Recommended comparison groups:

1. `00_baseline_rc_only` vs `01_proposed_default`: validates HBT-aware PDTreeRouter cost against weak baseline.
2. `01_proposed_default` vs `02_ablation_no_hbt_net_penalty`: isolates net-level HBT penalty.
3. `03_hbt_conservative` vs `04_timing_aggressive`: shows HBT resource vs timing tradeoff.
4. `05_high_hbt_rc` vs `06_low_hbt_rc`: validates HBT RC sensitivity / 3D awareness.
5. `07_wire_delay_dominant` and `08_parent_load_dominant`: validates timing-driven RC terms.
6. `09_no_stretch_penalty` vs `10_high_stretch_penalty`: isolates stretch control.
