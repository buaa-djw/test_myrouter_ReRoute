#include "NetInfoWriter.h"

#include <cmath>
#include <fstream>
#include <unordered_map>

namespace {

double segmentLengthDbu(const RoutedSegment& seg)
{
    return std::abs(seg.p1.x - seg.p2.x) + std::abs(seg.p1.y - seg.p2.y);
}

}  // namespace

void writeNetInfo(const std::string& path,
                  const RouterDB& db,
                  const std::vector<NetRouteResult>& results)
{
    std::ofstream o(path);
    std::unordered_map<std::string, int> net_pin_count;
    for (const auto& net : db.nets) {
        net_pin_count[net.name] = static_cast<int>(net.pins.size());
    }

    for (const auto& r : results) {
        double wirelength = 0.0;
        double top_wirelength = 0.0;
        double bottom_wirelength = 0.0;
        int hbt_count = 0;

        for (const auto& s : r.segments) {
            const double len = segmentLengthDbu(s);
            wirelength += len;
            if (s.uses_hbt) {
                ++hbt_count;
                continue;
            }
            if (s.p1.die == DieId::kTop) {
                top_wirelength += len;
            } else if (s.p1.die == DieId::kBottom) {
                bottom_wirelength += len;
            }
        }

        const int pin_count = net_pin_count.count(r.net_name)
                                  ? net_pin_count[r.net_name]
                                  : r.delay_summary.pin_count;

        o << "NET " << r.net_name << "\n";
        o << "  type: " << (r.is_3d ? "3D" : "2D") << "\n";
        o << "  cost_mode: " << r.cost_mode << "\n";
        o << "  pin_count: " << pin_count << "\n";
        o << "  success: " << r.success << "\n";
        o << "  status: " << r.status << "\n";
        o << "  fail_reason: " << r.fail_reason << "\n";
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
        o << "  max_delay_pin_index: " << r.delay_summary.max_delay_pin_index << "\n";
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
        o << "  reroute_touched: " << r.reroute_info.touched << "\n";
        o << "  reroute_improved: " << r.reroute_info.improved << "\n";
        o << "  reroute_edit_type: " << r.reroute_info.edit_type << "\n";
        o << "  reroute_delay_before: " << r.reroute_info.delay_before << "\n";
        o << "  reroute_delay_after: " << r.reroute_info.delay_after << "\n";
        o << "  reroute_objective_before: " << r.reroute_info.objective_before << "\n";
        o << "  reroute_objective_after: " << r.reroute_info.objective_after << "\n";
        o << "  reroute_wirelength_before: " << r.reroute_info.wirelength_before << "\n";
        o << "  reroute_wirelength_after: " << r.reroute_info.wirelength_after << "\n";
        o << "  reroute_hbt_count_before: " << r.reroute_info.hbt_count_before << "\n";
        o << "  reroute_hbt_count_after: " << r.reroute_info.hbt_count_after << "\n";
        o << "  reroute_reject_reason: " << r.reroute_info.reject_reason << "\n";
        o << "  reroute_old_hbt_id: " << r.reroute_info.old_hbt_id << "\n";
        o << "  reroute_new_hbt_id: " << r.reroute_info.new_hbt_id << "\n";
        o << "  reroute_changed_hbt_id_count: " << r.reroute_info.changed_hbt_id_count << "\n";
        o << "  reroute_changed_segment_count: " << r.reroute_info.changed_segment_count << "\n";
        o << "  reroute_force_accepted: " << r.reroute_info.force_accepted << "\n";

        o << "  tree_nodes: " << r.tree_nodes.size() << "\n";
        o << "  segments: " << r.segments.size() << "\n\n";
    }
}
