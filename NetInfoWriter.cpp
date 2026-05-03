#include "NetInfoWriter.h"

#include <cmath>
#include <fstream>

void writeNetInfo(const std::string& path, const RouterDB&, const std::vector<NetRouteResult>& results)
{
    std::ofstream o(path);
    for (const auto& r : results) {
        double wirelength = 0.0;
        double top_wirelength = 0.0;
        double bottom_wirelength = 0.0;
        int hbt_count = 0;
        for (const auto& s : r.segments) {
            wirelength += std::abs(s.p1.x - s.p2.x) + std::abs(s.p1.y - s.p2.y);
            if (!s.uses_hbt) {
                if (s.p1.die == DieId::kTop) {
                    top_wirelength += std::abs(s.p1.x - s.p2.x) + std::abs(s.p1.y - s.p2.y);
                } else if (s.p1.die == DieId::kBottom) {
                    bottom_wirelength += std::abs(s.p1.x - s.p2.x) + std::abs(s.p1.y - s.p2.y);
                }
            }
            if (s.uses_hbt) {
                ++hbt_count;
            }
        }
        o << "NET " << r.net_name << "\n";
        o << "  type: " << (r.is_3d ? "3D" : "2D") << "\n";
        o << "  cost_mode: " << r.cost_mode << "\n";
        o << "  pin_count: " << r.delay_summary.expected_sink_count + 1 << "\n";
        o << "  success: " << r.success << "\n";
        o << "  status: " << r.status << "\n";
        o << "  route_cost_total: " << r.route_cost_total << "\n";
        o << "  route_wire_delay: " << r.route_wire_delay << "\n";
        o << "  route_parent_load_delay: " << r.route_parent_load_delay << "\n";
        o << "  route_hbt_rc_delay: " << r.route_hbt_rc_delay << "\n";
        o << "  route_hbt_net_penalty_delay: " << r.route_hbt_net_penalty_delay << "\n";
        o << "  route_hbt_net_quad_penalty_delay: " << r.route_hbt_net_quad_penalty_delay << "\n";
        o << "  route_hbt_path_penalty_delay: " << r.route_hbt_path_penalty_delay << "\n";
        o << "  route_stretch_penalty_delay: " << r.route_stretch_penalty_delay << "\n";
        o << "  wirelength: " << wirelength << "\n";
        o << "  top_wirelength: " << top_wirelength << "\n";
        o << "  bottom_wirelength: " << bottom_wirelength << "\n";
        o << "  hbt_count: " << hbt_count << "\n";
        o << "  avg_sink_delay: " << r.delay_summary.avg_sink_delay << "\n";
        o << "  max_sink_delay: " << r.delay_summary.max_sink_delay << "\n";
        o << "  delay_ready: " << r.delay_summary.ready << "\n";
        o << "  delay_status: " << r.delay_summary.status << "\n";
        o << "  delay_fail_reason: " << r.delay_summary.fail_reason << "\n";
        o << "  expected_sink_count: " << r.delay_summary.expected_sink_count << "\n";
        o << "  mapped_sink_count: " << r.delay_summary.mapped_sink_count << "\n";
        o << "  unmapped_sink_count: " << r.delay_summary.unmapped_sink_count << "\n";
        o << "  single_pin_net: " << r.delay_summary.single_pin_net << "\n";
        o << "  total_wire_cap: " << r.delay_summary.total_wire_cap << "\n";
        o << "  total_load_cap: " << r.delay_summary.total_load_cap << "\n";
        o << "  total_hbt_cap: " << r.delay_summary.total_hbt_cap << "\n";
        o << "  total_tree_cap: " << r.delay_summary.total_tree_cap << "\n";
        o << "  ed_driver_delay_contrib: " << r.delay_summary.ed_driver_delay_contrib << "\n";
        o << "  ed_wire_delay_contrib: " << r.delay_summary.ed_wire_delay_contrib << "\n";
        o << "  ed_hbt_delay_contrib: " << r.delay_summary.ed_hbt_delay_contrib << "\n";
        o << "  ed_total_delay_contrib: " << r.delay_summary.ed_total_delay_contrib << "\n";
        o << "  max_delay_pin_name: " << r.delay_summary.max_delay_pin_name << "\n";
        o << "  validation: " << (r.validation.valid ? "OK" : "INVALID") << "\n";
        o << "  tree_nodes: " << r.tree_nodes.size() << "\n";
        o << "  segments: " << r.segments.size() << "\n\n";
    }
}
