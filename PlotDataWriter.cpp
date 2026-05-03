#include "PlotDataWriter.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

std::string safeNetName(const std::string& name)
{
    std::string out = name;
    for (char& c : out) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
            c = '_';
        }
    }
    return out;
}

namespace {

double dieToZ(DieId die)
{
    if (die == DieId::kTop) {
        return 1.0;
    }
    if (die == DieId::kBottom) {
        return 0.0;
    }
    return 0.5;
}

int computeWirelength(const NetRouteResult& result)
{
    int wl = 0;
    for (const RoutedSegment& s : result.segments) {
        wl += std::abs(s.p1.x - s.p2.x) + std::abs(s.p1.y - s.p2.y);
    }
    return wl;
}

}  // namespace

void writeNetPlotData(const std::string& root_dir, const NetRouteResult& result)
{
    const std::filesystem::path out_dir = std::filesystem::path(root_dir);
    std::filesystem::create_directories(out_dir);

    json j;
    j["net_name"] = result.net_name;
    j["is_3d"] = result.is_3d;
    j["success"] = result.success;
    j["status"] = result.status;
    j["fail_reason"] = result.fail_reason;
    j["route_cost_total"] = result.route_cost_total;
    j["route_wire_delay"] = result.route_wire_delay;
    j["route_parent_load_delay"] = result.route_parent_load_delay;
    j["route_hbt_rc_delay"] = result.route_hbt_rc_delay;
    j["route_hbt_net_penalty_delay"] = result.route_hbt_net_penalty_delay;
    j["route_hbt_net_quad_penalty_delay"] = result.route_hbt_net_quad_penalty_delay;
    j["route_hbt_path_penalty_delay"] = result.route_hbt_path_penalty_delay;
    j["route_stretch_penalty_delay"] = result.route_stretch_penalty_delay;
    j["wirelength"] = computeWirelength(result);
    j["cost_mode"] = "unknown";
    int top_wl = 0;
    int bottom_wl = 0;
    int hbt_count = 0;
    for (const RoutedSegment& s : result.segments) {
        if (s.uses_hbt) {
            ++hbt_count;
            continue;
        }
        const int wl = std::abs(s.p1.x - s.p2.x) + std::abs(s.p1.y - s.p2.y);
        if (s.p1.die == DieId::kTop) {
            top_wl += wl;
        } else if (s.p1.die == DieId::kBottom) {
            bottom_wl += wl;
        }
    }
    j["top_wirelength"] = top_wl;
    j["bottom_wirelength"] = bottom_wl;
    j["hbt_count"] = hbt_count;
    j["avg_sink_delay"] = result.delay_summary.avg_sink_delay;
    j["max_sink_delay"] = result.delay_summary.max_sink_delay;
    j["delay_ready"] = result.delay_summary.ready;
    j["validation"] = {{"valid", result.validation.valid}, {"errors", result.validation.errors}};
    j["points"] = json::array();
    j["segments"] = json::array();

    for (size_t i = 0; i < result.tree_nodes.size(); ++i) {
        const auto& n = result.tree_nodes[i];
        j["points"].push_back({{"id", static_cast<int>(i)}, {"x", n.point.x}, {"y", n.point.y}, {"z", dieToZ(n.point.die)}});
    }
    for (const auto& s : result.segments) {
        j["segments"].push_back({{"x1", s.p1.x}, {"y1", s.p1.y}, {"z1", dieToZ(s.p1.die)}, {"x2", s.p2.x}, {"y2", s.p2.y}, {"z2", dieToZ(s.p2.die)}, {"uses_hbt", s.uses_hbt}, {"hbt_id", s.hbt_id}});
    }

    std::ofstream ofs(out_dir / (safeNetName(result.net_name) + ".json"));
    ofs << j.dump(2);
}

void write3DNetPlotData(const std::string& dir, const NetRouteResult& result)
{
    // Deprecated compatibility wrapper.
    writeNetPlotData(dir, result);
}
