#include "ExperimentSummaryWriter.h"

#include <fstream>
#include <limits>

void writeExperimentSummary(const std::string& path,
                            const ExperimentConfig& cfg,
                            const RouterDB& db,
                            const std::vector<NetRouteResult>& results,
                            const std::string& st,
                            const std::string& et,
                            double rt,
                            const CriticalNetOptimizerStatsProxy* reroute_stats)
{
    std::ofstream o(path);
    int routed_success = 0;
    int routed_2d = 0;
    int routed_3d = 0;
    int topology_valid = 0;
    int delay_ready = 0;
    int delay_failed = 0;
    int single_pin_nets = 0;
    int delay_effective_sink_nets = 0;
    int ed_3d_hbt_contrib_zero_count = 0;
    int count_2d = 0, count_3d = 0;
    double sum_2d = 0.0, sum_3d = 0.0, max_2d = 0.0, max_3d = 0.0;
    int n_2d = 0, n_3d = 0;
    double ed_total_driver = 0.0, ed_total_wire = 0.0, ed_total_hbt = 0.0, ed_total = 0.0;
    double ed_sum_hbt_3d = 0.0, ed_max_hbt_3d = 0.0;
    int ed_hbt_3d_n = 0;
    double total_cost = 0.0;
    double total_wl = 0.0;
    int total_hbt = 0;
    for (const auto& r : results) {
        routed_success += r.success ? 1 : 0;
        routed_2d += (!r.is_3d && r.success) ? 1 : 0;
        routed_3d += (r.is_3d && r.success) ? 1 : 0;
        topology_valid += r.validation.valid ? 1 : 0;
        delay_ready += r.delay_summary.ready ? 1 : 0;
        delay_failed += r.delay_summary.ready ? 0 : 1;
        count_3d += r.is_3d ? 1 : 0;
        count_2d += r.is_3d ? 0 : 1;
        single_pin_nets += r.delay_summary.single_pin_net ? 1 : 0;
        const bool has_effective_sink = r.delay_summary.expected_sink_count > 0;
        if (r.delay_summary.ready && has_effective_sink) {
            ++delay_effective_sink_nets;
            if (r.is_3d) {
                sum_3d += r.delay_summary.avg_sink_delay; n_3d++; max_3d = std::max(max_3d, r.delay_summary.max_sink_delay);
            } else {
                sum_2d += r.delay_summary.avg_sink_delay; n_2d++; max_2d = std::max(max_2d, r.delay_summary.max_sink_delay);
            }
        }
        ed_total_driver += r.delay_summary.ed_driver_delay_contrib;
        ed_total_wire += r.delay_summary.ed_wire_delay_contrib;
        ed_total_hbt += r.delay_summary.ed_hbt_delay_contrib;
        ed_total += r.delay_summary.ed_total_delay_contrib;
        if (r.is_3d && r.delay_summary.ready) {
            ed_sum_hbt_3d += r.delay_summary.ed_hbt_delay_contrib;
            ed_max_hbt_3d = std::max(ed_max_hbt_3d, r.delay_summary.ed_hbt_delay_contrib);
            ++ed_hbt_3d_n;
            if (r.delay_summary.ed_hbt_delay_contrib == 0.0) {
                ++ed_3d_hbt_contrib_zero_count;
            }
        }
        total_cost += r.route_cost_total;
        for (const auto& s : r.segments) {
            total_wl += std::abs(s.p1.x - s.p2.x) + std::abs(s.p1.y - s.p2.y);
            total_hbt += s.uses_hbt ? 1 : 0;
        }
    }
    o << "Experiment Info\n";
    o << "experiment_name=" << cfg.experiment_name << "\nbenchmark=" << cfg.benchmark << "\ncost_mode=" << cfg.cost_mode << "\nstart=" << st << "\nend=" << et << "\nruntime=" << rt << "\noutput_dir=" << cfg.output.output_dir << "\n\n";
    o << "RC/Cost Config\n";
    o << "default_sink_cap=" << cfg.rc.default_sink_cap << "\n";
    o << "effective_top_unit_res=" << db.computeEffectiveTopUnitRes() << "\n";
    o << "effective_top_unit_cap=" << db.computeEffectiveTopUnitCap() << "\n";
    o << "effective_bottom_unit_res=" << db.computeEffectiveBottomUnitRes() << "\n";
    o << "effective_bottom_unit_cap=" << db.computeEffectiveBottomUnitCap() << "\n";
    o << "top_wire_r_scale=" << cfg.rc.top_wire_r_scale << "\n";
    o << "top_wire_c_scale=" << cfg.rc.top_wire_c_scale << "\n";
    o << "bottom_wire_r_scale=" << cfg.rc.bottom_wire_r_scale << "\n";
    o << "bottom_wire_c_scale=" << cfg.rc.bottom_wire_c_scale << "\n";
    o << "directional_rc_enabled=true\n";
    o << "fallback_wire_res_per_um=" << cfg.report_cost.default_wire_res_per_um << "\n";
    o << "fallback_wire_cap_per_um=" << cfg.report_cost.default_wire_cap_per_um << "\n";
    o << "router_hbt_res=" << cfg.rc.hbt_res * cfg.rc.hbt_rc_scale << "\n";
    o << "router_hbt_cap=" << cfg.rc.hbt_cap * cfg.rc.hbt_rc_scale << "\n";
    o << "router_hbt_rc_scale=" << cfg.rc.hbt_rc_scale << "\n";
    o << "edcompute_rc_override=" << cfg.edcompute_rc.override_enable << "\n";
    o << "edcompute_override_tree_hbt_rc=" << cfg.edcompute_rc.override_enable << "\n";
    o << "edcompute_hbt_res=" << (cfg.edcompute_rc.override_enable ? cfg.edcompute_rc.hbt_res * cfg.edcompute_rc.hbt_rc_scale : cfg.rc.hbt_res * cfg.rc.hbt_rc_scale) << "\n";
    o << "edcompute_hbt_cap=" << (cfg.edcompute_rc.override_enable ? cfg.edcompute_rc.hbt_cap * cfg.edcompute_rc.hbt_rc_scale : cfg.rc.hbt_cap * cfg.rc.hbt_rc_scale) << "\n";
    o << "edcompute_hbt_rc_scale=" << (cfg.edcompute_rc.override_enable ? cfg.edcompute_rc.hbt_rc_scale : cfg.rc.hbt_rc_scale) << "\n";
    o << "dump_2d_plot_data=" << cfg.output.dump_2d_plot_data << "\n";
    o << "dump_3d_plot_data=" << cfg.output.dump_3d_plot_data << "\n\n";
    o << "Overall Metrics\n";
    o << "total_nets=" << results.size() << "\nrouted_success=" << routed_success << "\nrouted_failed=" << (static_cast<int>(results.size()) - routed_success) << "\nrouted_2d_nets=" << routed_2d << "\nrouted_3d_nets=" << routed_3d << "\nall_2d_nets=" << count_2d << "\nall_3d_nets=" << count_3d << "\ntopology_valid_nets=" << topology_valid << "\nsingle_pin_nets=" << single_pin_nets << "\ndelay_ready_nets=" << delay_ready << "\ndelay_failed_nets=" << delay_failed << "\ndelay_effective_sink_nets=" << delay_effective_sink_nets << "\n\n";
    o << "Aggregated Metrics\n";
    o << "total_wirelength=" << total_wl << "\ntotal_route_cost=" << total_cost << "\naverage_route_cost=" << (results.empty()?0.0:total_cost/results.size()) << "\ntotal_hbt_count=" << total_hbt << "\n\n";
    o << "EDCompute Metrics\n";
    o << "ed_total_driver_delay_contrib=" << ed_total_driver << "\n";
    o << "ed_total_wire_delay_contrib=" << ed_total_wire << "\n";
    o << "ed_total_hbt_delay_contrib=" << ed_total_hbt << "\n";
    o << "ed_total_delay_contrib=" << ed_total << "\n";
    o << "ed_avg_hbt_delay_contrib_for_3d_nets=" << (ed_hbt_3d_n ? ed_sum_hbt_3d / ed_hbt_3d_n : 0.0) << "\n";
    o << "ed_max_hbt_delay_contrib_for_3d_nets=" << ed_max_hbt_3d << "\n";
    o << "ed_3d_hbt_contrib_zero_count=" << ed_3d_hbt_contrib_zero_count << "\n";
    o << "avg_3d_sink_delay=" << (n_3d ? sum_3d / n_3d : 0.0) << "\n";
    o << "max_3d_sink_delay=" << max_3d << "\n";
    o << "avg_2d_sink_delay=" << (n_2d ? sum_2d / n_2d : 0.0) << "\n";
    o << "max_2d_sink_delay=" << max_2d << "\n";
    if (reroute_stats) {
        o << "\nReRoute Metrics\n";
        o << "visited_nets=" << reroute_stats->visited_nets << "\n";
        o << "reroute_target_net_type=" << cfg.reroute.target_net_type << "\n";
        o << "reroute_visited_2d_nets=" << reroute_stats->visited_2d_nets << "\n";
        o << "reroute_visited_3d_nets=" << reroute_stats->visited_3d_nets << "\n";
        o << "improved_nets=" << reroute_stats->improved_nets << "\n";
        o << "tried_candidates=" << reroute_stats->tried_candidates << "\n";
        o << "accepted_candidates=" << reroute_stats->accepted_candidates << "\n";
        o << "rejected_by_topology=" << reroute_stats->rejected_by_topology << "\n";
        o << "rejected_by_delay=" << reroute_stats->rejected_by_delay << "\n";
        o << "rejected_by_wirelength=" << reroute_stats->rejected_by_wirelength << "\n";
        o << "rejected_by_hbt_conflict=" << reroute_stats->rejected_by_hbt_conflict << "\n";
        o << "tried_reattach_candidates=" << reroute_stats->tried_reattach_candidates << "\n";
        o << "tried_ripup_candidates=" << reroute_stats->tried_ripup_candidates << "\n";
        o << "accepted_reattach_candidates=" << reroute_stats->accepted_reattach_candidates << "\n";
        o << "accepted_ripup_candidates=" << reroute_stats->accepted_ripup_candidates << "\n";
        o << "rejected_by_cycle=" << reroute_stats->rejected_by_cycle << "\n";
        o << "rejected_by_cross_die_not_supported=" << reroute_stats->rejected_by_cross_die_not_supported << "\n";
        o << "rejected_by_invalid_hbt=" << reroute_stats->rejected_by_invalid_hbt << "\n";
        o << "tried_hbt_swap_candidates=" << reroute_stats->tried_hbt_swap_candidates << "\n";
        o << "accepted_hbt_swap_candidates=" << reroute_stats->accepted_hbt_swap_candidates << "\n";
        o << "tried_cross_die_ripup_candidates=" << reroute_stats->tried_cross_die_ripup_candidates << "\n";
        o << "accepted_cross_die_ripup_candidates=" << reroute_stats->accepted_cross_die_ripup_candidates << "\n";
        o << "tried_hbt_insert_candidates=" << reroute_stats->tried_hbt_insert_candidates << "\n";
        o << "accepted_hbt_insert_candidates=" << reroute_stats->accepted_hbt_insert_candidates << "\n";
        o << "tried_hbt_remove_candidates=" << reroute_stats->tried_hbt_remove_candidates << "\n";
        o << "accepted_hbt_remove_candidates=" << reroute_stats->accepted_hbt_remove_candidates << "\n";
        o << "rejected_by_no_free_hbt=" << reroute_stats->rejected_by_no_free_hbt << "\n";
        o << "rejected_by_no_hbt_on_path=" << reroute_stats->rejected_by_no_hbt_on_path << "\n";
        o << "rejected_by_non_3d_net=" << reroute_stats->rejected_by_non_3d_net << "\n";
        o << "rejected_by_build_hbt_branch_failed=" << reroute_stats->rejected_by_build_hbt_branch_failed << "\n";
        o << "rejected_by_hbt_swap_not_applied=" << reroute_stats->rejected_by_hbt_swap_not_applied << "\n";
        o << "changed_hbt_id_count=" << reroute_stats->changed_hbt_id_count << "\n";
        o << "changed_hbt_count_total=" << reroute_stats->changed_hbt_count_total << "\n";
        o << "hbt_count_before=" << reroute_stats->hbt_count_before << "\n";
        o << "hbt_count_after=" << reroute_stats->hbt_count_after << "\n";
        o << "hbt_conflict_before=" << reroute_stats->hbt_conflict_before << "\n";
        o << "hbt_conflict_after=" << reroute_stats->hbt_conflict_after << "\n";
        o << "hbt_swap_force_accept_used=" << reroute_stats->hbt_swap_force_accept_used << "\n";
    }
}
