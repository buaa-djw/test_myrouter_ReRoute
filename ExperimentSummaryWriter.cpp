#include "ExperimentSummaryWriter.h"

#include <fstream>

void writeExperimentSummary(const std::string& path,
                            const ExperimentConfig& cfg,
                            const RouterDB& db,
                            const std::vector<NetRouteResult>& results,
                            const std::string& st,
                            const std::string& et,
                            double rt)
{
    std::ofstream o(path);
    int routed_success = 0;
    int routed_2d = 0;
    int routed_3d = 0;
    int topology_valid = 0;
    int delay_ready = 0;
    double total_cost = 0.0;
    double total_wl = 0.0;
    int total_hbt = 0;
    for (const auto& r : results) {
        routed_success += r.success ? 1 : 0;
        routed_2d += (!r.is_3d && r.success) ? 1 : 0;
        routed_3d += (r.is_3d && r.success) ? 1 : 0;
        topology_valid += r.validation.valid ? 1 : 0;
        delay_ready += r.delay_summary.ready ? 1 : 0;
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
    o << "dump_2d_plot_data=" << cfg.output.dump_2d_plot_data << "\n";
    o << "dump_3d_plot_data=" << cfg.output.dump_3d_plot_data << "\n\n";
    o << "Overall Metrics\n";
    o << "total_nets=" << results.size() << "\nrouted_success=" << routed_success << "\nrouted_failed=" << (static_cast<int>(results.size()) - routed_success) << "\nrouted_2d_nets=" << routed_2d << "\nrouted_3d_nets=" << routed_3d << "\ntopology_valid_nets=" << topology_valid << "\ndelay_ready_nets=" << delay_ready << "\n\n";
    o << "Aggregated Metrics\n";
    o << "total_wirelength=" << total_wl << "\ntotal_route_cost=" << total_cost << "\naverage_route_cost=" << (results.empty()?0.0:total_cost/results.size()) << "\ntotal_hbt_count=" << total_hbt << "\n\n";
}
