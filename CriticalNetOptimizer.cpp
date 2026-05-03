#include "CriticalNetOptimizer.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <queue>
#include <unordered_map>

namespace {

DieId normalizeDieLocal(DieId d)
{
    if (d == DieId::kTop || d == DieId::kBottom) {
        return d;
    }
    return DieId::kTop;
}

}  // namespace

CriticalNetOptimizer::CriticalNetOptimizer(const RouterDB& db,
                                           const HybridGrid& grid,
                                           const PDTreeRouter& router,
                                           const Params& params)
    : db_(db), grid_(grid), router_(router), params_(params)
{
}

CriticalNetOptimizer::OptimizationStats CriticalNetOptimizer::optimize(
    std::vector<NetRouteResult>& results) const
{
    OptimizationStats stats;

    const std::vector<CriticalNetRecord> critical_nets = collectCriticalNets(results);
    for (const CriticalNetRecord& rec : critical_nets) {
        if (rec.result_index < 0 || rec.result_index >= static_cast<int>(results.size())) {
            continue;
        }
        if (rec.net_index < 0 || rec.net_index >= static_cast<int>(db_.nets.size())) {
            continue;
        }

        NetRouteResult& result = results[rec.result_index];
        const Net& net = db_.nets[rec.net_index];
        if (!result.delay_summary.ready) {
            continue;
        }

        ++stats.visited_nets;
        stats.total_avg_delay_before += result.delay_summary.avg_sink_delay;
        stats.total_max_delay_before += result.delay_summary.max_sink_delay;

        const bool improved = optimizeOneNet(net, result);
        if (improved) {
            ++stats.improved_nets;
        }

        stats.total_avg_delay_after += result.delay_summary.avg_sink_delay;
        stats.total_max_delay_after += result.delay_summary.max_sink_delay;
    }

    if (params_.verbose) {
        std::cout << "[CriticalNetOptimizer] done visited=" << stats.visited_nets
                  << " improved=" << stats.improved_nets
                  << " avg_delay " << stats.total_avg_delay_before << " -> "
                  << stats.total_avg_delay_after
                  << " max_delay " << stats.total_max_delay_before << " -> "
                  << stats.total_max_delay_after << std::endl;
    }

    return stats;
}

std::vector<CriticalNetOptimizer::CriticalNetRecord> CriticalNetOptimizer::collectCriticalNets(
    const std::vector<NetRouteResult>& results) const
{
    std::unordered_map<std::string, int> net_name_to_idx;
    net_name_to_idx.reserve(db_.nets.size());
    for (int i = 0; i < static_cast<int>(db_.nets.size()); ++i) {
        net_name_to_idx[db_.nets[i].name] = i;
    }

    std::vector<CriticalNetRecord> records;
    for (int ridx = 0; ridx < static_cast<int>(results.size()); ++ridx) {
        const NetRouteResult& result = results[ridx];
        if (!result.success || !result.delay_summary.ready) {
            continue;
        }

        auto it = net_name_to_idx.find(result.net_name);
        if (it == net_name_to_idx.end()) {
            continue;
        }

        const Net& net = db_.nets[it->second];
        if (net.net_class != NetClass::kNormal) {
            continue;
        }

        CriticalNetRecord rec;
        rec.result_index = ridx;
        rec.net_index = it->second;
        rec.net_name = result.net_name;
        rec.avg_delay = result.delay_summary.avg_sink_delay;
        rec.max_delay = result.delay_summary.max_sink_delay;
        rec.max_pin_index = result.delay_summary.max_delay_pin_index;
        records.push_back(rec);
    }

    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
        constexpr double kEps = 1e-15;
        if (a.max_delay > b.max_delay + kEps) {
            return true;
        }
        if (b.max_delay > a.max_delay + kEps) {
            return false;
        }
        return a.avg_delay > b.avg_delay;
    });

    const int keep = std::max(0, params_.top_k_nets);
    if (static_cast<int>(records.size()) > keep) {
        records.resize(keep);
    }

    if (params_.verbose) {
        std::cout << "[CriticalNetOptimizer] top critical nets:";
        for (const auto& rec : records) {
            std::cout << " {" << rec.net_name
                      << ": avg=" << rec.avg_delay
                      << ", max=" << rec.max_delay << "}";
        }
        std::cout << std::endl;
    }

    return records;
}

bool CriticalNetOptimizer::optimizeOneNet(const Net& net, NetRouteResult& result) const
{
    if (!result.success || !result.delay_summary.ready) {
        return false;
    }

    const int max_pin = result.delay_summary.max_delay_pin_index;
    const int sink_tree = findTreeNodeForPin(result, max_pin);
    if (sink_tree < 0 || sink_tree == result.root_tree_index) {
        return false;
    }

    const Pin& sink_pin = net.pins[max_pin];
    const DieId sink_die = normalizeDieLocal(router_.pinToPointPublic(sink_pin).die);

    const double base_obj = evaluatePostOptimizationObjective(result, net);
    if (params_.verbose) {
        std::cout << "[CriticalNetOptimizer] net=" << net.name
                  << " max_delay_pin=" << max_pin
                  << " base_obj=" << base_obj << std::endl;
    }

    PDTreeRouter::TimingSummary base_timing = router_.evaluateTimingSummaryPublic(net, result);
    const int base_hbt = base_timing.hbt_count;
    const double base_wl = base_timing.total_wirelength;

    const int old_parent = result.tree_nodes[sink_tree].parent_index;
    std::vector<int> parent_candidates = router_.collectCandidateParentsPublic(net, result, sink_pin);

    NetRouteResult best = result;
    double best_obj = base_obj;
    bool found_better = false;

    int tried = 0;
    for (int cand_parent : parent_candidates) {
        if (tried >= params_.max_iterations_per_net) {
            break;
        }
        if (cand_parent < 0 || cand_parent >= static_cast<int>(result.tree_nodes.size())) {
            continue;
        }
        if (cand_parent == sink_tree || cand_parent == old_parent) {
            continue;
        }
        if (isInSubtree(result, sink_tree, cand_parent)) {
            continue;
        }

        const DieId parent_die = normalizeDieLocal(result.tree_nodes[cand_parent].point.die);
        if (parent_die != sink_die) {
            continue;
        }

        std::vector<RoutedSegment> new_segments;
        if (!router_.build2DConnectionPublic(result.tree_nodes[cand_parent].point,
                                             router_.pinToPointPublic(sink_pin),
                                             new_segments)) {
            continue;
        }

        ++tried;
        NetRouteResult tmp = result;
        if (!replaceSinkIncomingBranch(net,
                                       tmp,
                                       sink_tree,
                                       cand_parent,
                                       new_segments,
                                       router_.pinToPointPublic(sink_pin))) {
            continue;
        }
        if (!router_.annotateDelayPublic(net, tmp) || !tmp.delay_summary.ready) {
            continue;
        }

        PDTreeRouter::TimingSummary tmp_timing = router_.evaluateTimingSummaryPublic(net, tmp);
        const bool wl_ok = tmp_timing.total_wirelength <= base_wl * (1.0 + params_.max_wirelength_growth_ratio) + 1e-9;
        const bool hbt_ok = tmp_timing.hbt_count <= base_hbt + params_.max_extra_hbts;
        if (!wl_ok || !hbt_ok) {
            continue;
        }

        const bool better_delay =
            (tmp.delay_summary.max_sink_delay + 1e-12 < result.delay_summary.max_sink_delay)
            || (std::abs(tmp.delay_summary.max_sink_delay - result.delay_summary.max_sink_delay) <= 1e-12
                && tmp.delay_summary.avg_sink_delay + 1e-12 < result.delay_summary.avg_sink_delay);
        if (!better_delay) {
            continue;
        }

        const double obj = evaluatePostOptimizationObjective(tmp, net);
        if (obj + 1e-12 < best_obj) {
            best = std::move(tmp);
            best_obj = obj;
            found_better = true;
        }
    }

    // TODO: generate and evaluate EditType::kSwapHBT in a future revision.
    // TODO: generate and evaluate EditType::kRipupOneSinkBranch in a future revision.

    if (found_better) {
        result = std::move(best);
    }

    if (params_.verbose) {
        std::cout << "[CriticalNetOptimizer] net=" << net.name
                  << " improved=" << (found_better ? "true" : "false")
                  << " obj " << base_obj << " -> " << evaluatePostOptimizationObjective(result, net)
                  << std::endl;
    }

    return found_better;
}

int CriticalNetOptimizer::findTreeNodeForPin(const NetRouteResult& result, int pin_index) const
{
    for (int i = 0; i < static_cast<int>(result.tree_nodes.size()); ++i) {
        if (result.tree_nodes[i].pin_index == pin_index) {
            return i;
        }
    }
    return -1;
}

bool CriticalNetOptimizer::isInSubtree(const NetRouteResult& result, int root_node, int query_node) const
{
    if (root_node < 0 || root_node >= static_cast<int>(result.tree_nodes.size())) {
        return false;
    }
    if (query_node < 0 || query_node >= static_cast<int>(result.tree_nodes.size())) {
        return false;
    }

    std::vector<std::vector<int>> children(result.tree_nodes.size());
    for (int i = 0; i < static_cast<int>(result.tree_nodes.size()); ++i) {
        if (i == result.root_tree_index) {
            continue;
        }
        const int p = result.tree_nodes[i].parent_index;
        if (p >= 0 && p < static_cast<int>(result.tree_nodes.size())) {
            children[p].push_back(i);
        }
    }

    std::queue<int> q;
    q.push(root_node);
    while (!q.empty()) {
        const int u = q.front();
        q.pop();
        if (u == query_node) {
            return true;
        }
        for (int v : children[u]) {
            q.push(v);
        }
    }

    return false;
}

bool CriticalNetOptimizer::replaceSinkIncomingBranch(const Net& net,
                                                     NetRouteResult& result,
                                                     int sink_tree_node,
                                                     int new_parent_tree_index,
                                                     const std::vector<RoutedSegment>& new_segments,
                                                     const RoutedPoint& sink_point) const
{
    (void) net;

    if (sink_tree_node < 0 || sink_tree_node >= static_cast<int>(result.tree_nodes.size())) {
        return false;
    }
    if (new_parent_tree_index < 0 || new_parent_tree_index >= static_cast<int>(result.tree_nodes.size())) {
        return false;
    }

    TreeNodeState& sink_node = result.tree_nodes[sink_tree_node];
    if (sink_node.incoming_segment_begin < 0 || sink_node.incoming_segment_count < 0) {
        return false;
    }

    const int old_begin = sink_node.incoming_segment_begin;
    const int old_count = sink_node.incoming_segment_count;
    const int old_end = old_begin + old_count;

    if (old_end > static_cast<int>(result.segments.size())) {
        return false;
    }

    std::vector<RoutedSegment> rebuilt;
    rebuilt.reserve(result.segments.size() - old_count + new_segments.size());

    rebuilt.insert(rebuilt.end(), result.segments.begin(), result.segments.begin() + old_begin);
    rebuilt.insert(rebuilt.end(), new_segments.begin(), new_segments.end());
    rebuilt.insert(rebuilt.end(), result.segments.begin() + old_end, result.segments.end());

    const int delta = static_cast<int>(new_segments.size()) - old_count;

    for (int i = 0; i < static_cast<int>(result.tree_nodes.size()); ++i) {
        TreeNodeState& node = result.tree_nodes[i];
        if (i == sink_tree_node) {
            continue;
        }
        if (node.incoming_segment_begin >= old_end) {
            node.incoming_segment_begin += delta;
        }
    }

    sink_node.parent_index = new_parent_tree_index;
    sink_node.incoming_segment_count = static_cast<int>(new_segments.size());
    sink_node.point = sink_point;

    sink_node.incoming_wire_length = 0;
    sink_node.incoming_hbt_count = 0;
    sink_node.incoming_wire_res = 0.0;
    sink_node.incoming_wire_cap = 0.0;
    sink_node.incoming_hbt_res = 0.0;
    sink_node.incoming_hbt_cap = 0.0;

    for (const RoutedSegment& seg : new_segments) {
        if (seg.uses_hbt) {
            ++sink_node.incoming_hbt_count;
            continue;
        }
        const int length_dbu = std::abs(seg.p1.x - seg.p2.x) + std::abs(seg.p1.y - seg.p2.y);
        sink_node.incoming_wire_length += length_dbu;

        const EffectiveRC rc = db_.computeEffectiveRCForDie(normalizeDieLocal(seg.p1.die));
        const double len_um = db_.dbuToMicronLength(length_dbu);
        sink_node.incoming_wire_res += rc.unit_res * len_um;
        sink_node.incoming_wire_cap += rc.unit_cap * len_um;
    }

    sink_node.incoming_segment_begin = old_begin;
    result.segments = std::move(rebuilt);
    return rebuildTreeStatistics(net, result);
}

bool CriticalNetOptimizer::rebuildTreeStatistics(const Net& /*net*/, NetRouteResult& result) const
{
    if (result.root_tree_index < 0 || result.root_tree_index >= static_cast<int>(result.tree_nodes.size())) {
        return false;
    }

    std::vector<std::vector<int>> children(result.tree_nodes.size());
    for (int i = 0; i < static_cast<int>(result.tree_nodes.size()); ++i) {
        if (i == result.root_tree_index) {
            continue;
        }
        const int p = result.tree_nodes[i].parent_index;
        if (p < 0 || p >= static_cast<int>(result.tree_nodes.size())) {
            return false;
        }
        children[p].push_back(i);
    }

    std::vector<int> order;
    order.reserve(result.tree_nodes.size());
    std::queue<int> q;
    q.push(result.root_tree_index);
    result.tree_nodes[result.root_tree_index].depth = 0;
    result.tree_nodes[result.root_tree_index].path_length_from_root = 0;
    result.tree_nodes[result.root_tree_index].hbt_count_from_root = 0;

    while (!q.empty()) {
        const int u = q.front();
        q.pop();
        order.push_back(u);

        for (int v : children[u]) {
            TreeNodeState& child = result.tree_nodes[v];
            const TreeNodeState& parent = result.tree_nodes[u];
            child.depth = parent.depth + 1;
            child.path_length_from_root = parent.path_length_from_root + child.incoming_wire_length;
            child.hbt_count_from_root = parent.hbt_count_from_root + child.incoming_hbt_count;
            q.push(v);
        }
    }

    return order.size() == result.tree_nodes.size();
}

double CriticalNetOptimizer::evaluatePostOptimizationObjective(const NetRouteResult& result,
                                                               const Net& net) const
{
    if (!result.success || !result.delay_summary.ready) {
        return std::numeric_limits<double>::infinity();
    }
    return router_.evaluateObjectivePublic(net, result);
}

std::vector<CriticalNetOptimizer::NetEditCandidate> CriticalNetOptimizer::generateHBTSwapCandidates(
    const Net& /*net*/,
    const NetRouteResult& /*result*/,
    int /*sink_tree_node*/) const
{
    return {};
}
