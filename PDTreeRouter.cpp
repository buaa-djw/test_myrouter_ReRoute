#include "PDTreeRouter.h"

#include "EDCompute.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <queue>
#include <sstream>
#include <utility>
#include <unordered_map>
#include <vector>

namespace {

int clampInt(int v, int lo, int hi)
{
    return std::max(lo, std::min(v, hi));
}

int hpwl(const Net& net)
{
    if (net.pins.empty()) {
        return 0;
    }

    int lx = std::numeric_limits<int>::max();
    int ly = std::numeric_limits<int>::max();
    int ux = std::numeric_limits<int>::min();
    int uy = std::numeric_limits<int>::min();

    for (const auto& p : net.pins) {
        lx = std::min(lx, p.x);
        ly = std::min(ly, p.y);
        ux = std::max(ux, p.x);
        uy = std::max(uy, p.y);
    }

    return (ux - lx) + (uy - ly);
}

DieId normalizeDie(DieId d)
{
    if (d == DieId::kTop || d == DieId::kBottom) {
        return d;
    }
    return DieId::kTop;
}

const char* dieToString(DieId d)
{
    const DieId n = normalizeDie(d);
    return (n == DieId::kTop) ? "top" : "bottom";
}

void appendPDRouteDebugLine(const std::string& line)
{
    std::ofstream ofs("pdroute_debug.rpt", std::ios::app);
    if (!ofs) {
        return;
    }
    ofs << line << '\n';
}

int countTreeNodeChildren(const NetRouteResult& result, int parent_tree_index)
{
    if (parent_tree_index < 0
        || parent_tree_index >= static_cast<int>(result.tree_nodes.size())) {
        return 0;
    }

    int child_count = 0;
    for (const TreeNodeState& node : result.tree_nodes) {
        if (node.parent_index == parent_tree_index) {
            ++child_count;
        }
    }
    return child_count;
}

int countHBTSegments(const NetRouteResult& result)
{
    int hbt_count = 0;
    for (const RoutedSegment& segment : result.segments) {
        if (segment.uses_hbt) {
            ++hbt_count;
        }
    }
    return hbt_count;
}

double totalWireLength(const std::vector<RoutedSegment>& segments)
{
    double wirelength = 0.0;
    for (const RoutedSegment& segment : segments) {
        if (segment.uses_hbt) {
            continue;
        }
        wirelength += std::abs(segment.p1.x - segment.p2.x)
                    + std::abs(segment.p1.y - segment.p2.y);
    }
    return wirelength;
}

const char* costModeToString(PDTreeRouter::CostMode mode)
{
    switch (mode) {
    case PDTreeRouter::CostMode::kTraditionalPDTree:
        return "traditional_pdtree";
    case PDTreeRouter::CostMode::kBaselineRcOnly:
        return "baseline_rc_only";
    case PDTreeRouter::CostMode::kProposed:
    default:
        return "proposed";
    }
}

}  // namespace

PDTreeRouter::PDTreeRouter(const RouterDB& db,
                           const HybridGrid& grid,
                           const Params& params)
     : db_(db), grid_(grid), params_(params)
{
    const std::size_t hbt_n = grid_.hbt.getSlots().size();
    hbt_used_.assign(hbt_n, false);
    hbt_owner_.assign(hbt_n, "");
}


PDTreeRouter::PDTreeRouter(const RouterDB& db,
                           const HybridGrid& grid)
    : PDTreeRouter(db, grid, Params{})
{
}

std::vector<NetRouteResult> PDTreeRouter::routeAllNets()
{
    RouteRunStats ignored;
    return routeSignalNets(-1, &ignored);
}

std::vector<NetRouteResult> PDTreeRouter::routeSignalNets(int max_nets,
                                                          RouteRunStats* stats)
{
    std::vector<int> order;
    order.reserve(db_.nets.size());
    for (int i = 0; i < static_cast<int>(db_.nets.size()); ++i) {
        const Net& net = db_.nets[i];
        if (!shouldRouteAsSignalNet(net)) {
            if (stats != nullptr) {
                if (net.net_class == NetClass::kClock) {
                    ++stats->skipped_clock;
                } else if (net.net_class == NetClass::kSpecial) {
                    ++stats->skipped_special;
                }
            }
            if (params_.verbose) {
                std::cout << "[PDTreeRouter] skip non-normal net=" << net.name
                          << " class=" << netClassToString(net.net_class) << std::endl;
            }
            continue;
        }
        order.push_back(i);
    }

    std::stable_sort(order.begin(), order.end(), [this](int a, int b) {
        return hpwl(db_.nets[a]) < hpwl(db_.nets[b]);
    });

    const int route_limit = (max_nets > 0)
                                ? std::min(max_nets, static_cast<int>(order.size()))
                                : static_cast<int>(order.size());

    std::vector<NetRouteResult> results;
    results.reserve(route_limit);

    for (int i = 0; i < route_limit; ++i) {
        NetRouteResult result = routeSingleNet(db_.nets[order[i]]);
        if (stats != nullptr) {
            if (result.success) {
                ++stats->routed_success;
            } else {
                ++stats->routed_failed;
            }
        }
        results.push_back(std::move(result));
    }

    return results;
}

bool PDTreeRouter::shouldRouteAsSignalNet(const Net& net) const
{
    return net.isSignalRoutable();
}

NetRouteResult PDTreeRouter::routeSingleNet(const Net& net)
{
    NetRouteResult skipped;
    skipped.net_name = net.name;
    skipped.is_3d = net.is_3d;

    if (!shouldRouteAsSignalNet(net)) {
        if (params_.verbose) {
            std::cout << "[PDTreeRouter] skip non-normal net=" << net.name
                      << " class=" << netClassToString(net.net_class) << std::endl;
        }
        return skipped;
    }

    if (net.is_3d) {
        return route3DNet(net);
    }
    return route2DNet(net);
}

PDTreeRouter::TimingSummary PDTreeRouter::evaluateTimingSummaryPublic(
    const Net& net,
    const NetRouteResult& result) const
{
    return evaluateTimingSummary(net, result);
}

double PDTreeRouter::evaluateObjectivePublic(const Net& net,
                                             const NetRouteResult& result) const
{
    (void) net;

    // Deprecated. Not used by RC-delay-driven PDTreeRouter cost.
    return result.route_cost_total;
}

bool PDTreeRouter::annotateDelayPublic(const Net& net,
                                       NetRouteResult& result) const
{
    EDCompute ed(db_, EDCompute::Params{params_.default_sink_cap,
                                        params_.source_res,
                                        params_.hbt_res,
                                        params_.hbt_cap,
                                        false});
    return ed.annotateNetDelay(net, result);
}

std::vector<int> PDTreeRouter::collectCandidateParentsPublic(
    const Net& net,
    const NetRouteResult& result,
    const Pin& sink) const
{
    return collectCandidateParents(net, result, sink);
}

bool PDTreeRouter::build2DConnectionPublic(const RoutedPoint& a,
                                           const RoutedPoint& b,
                                           std::vector<RoutedSegment>& out_segments) const
{
    return build2DManhattanConnection(a, b, out_segments);
}

RoutedPoint PDTreeRouter::pinToPointPublic(const Pin& pin) const
{
    return pinToPoint(pin);
}

NetRouteResult PDTreeRouter::route2DNet(const Net& net)
{
    NetRouteResult result;
    result.net_name = net.name;
    result.is_3d = false;
    result.root_tree_index = 0;

    if (net.pins.size() <= 1) {
        result.success = true;
        if (!net.pins.empty()) {
            TreeNodeState root;
            root.pin_index = 0;
            root.point = pinToPoint(net.pins[0]);
            root.node_type = TreeNodeState::NodeType::kPin;
            result.tree_nodes.push_back(root);
        }

        result.status = "ok";
        result.validation = validateRouteResultTopology(result);
        (void) annotateDelayPublic(net, result);
        return result;
    }

    const int src = chooseSourcePin(net);
    if (src < 0 || src >= static_cast<int>(net.pins.size())) {
        return result;
    }
    const DieId net_route_die = determine2DNetDie(net, src);
    if (params_.verbose) {
        std::cout << "[PDTreeRouter][2D] net=" << net.name
                  << " source_pin=" << net.pins[src].name
                  << " route_die=" << (net_route_die == DieId::kBottom ? "Bottom" : "Top")
                  << std::endl;
    }

    TreeNodeState root;
    root.pin_index = src;
    root.point = pinToPoint(net.pins[src]);
    root.point.die = resolve2DPinDie(net.pins[src], net_route_die);
    root.node_type = TreeNodeState::NodeType::kPin;
    result.tree_nodes.push_back(root);

    std::vector<bool> in_tree(net.pins.size(), false);
    in_tree[src] = true;

    for (int i = 0; i < static_cast<int>(net.pins.size()); ++i) {
        if (in_tree[i]) {
            continue;
        }
        if (!connectSinkSameDie(net, i, net_route_die, result, in_tree)) {
            if (params_.verbose) {
                std::cerr << "[PDTreeRouter][2D] failed net=" << net.name
                          << " sink_pin_index=" << i << std::endl;
            }
            result.success = false;
            return result;
        }
    }

    result.success = true;
    result.status = "ok";
    result.validation = validateRouteResultTopology(result);
    (void) annotateDelayPublic(net, result);

    if (params_.verbose) {
        const TimingSummary t = evaluateTimingSummary(net, result);
        std::cout << "[PDTreeRouter][timing] net=" << net.name
                  << " sinks=" << t.sink_count
                  << " avg_delay=" << t.avg_sink_delay
                  << " max_delay=" << t.max_sink_delay
                  << " wl=" << t.total_wirelength
                  << " hbt=" << t.hbt_count << std::endl;
    }
    return result;
}

NetRouteResult PDTreeRouter::route3DNet(const Net& net)
{
    NetRouteResult empty;
    empty.net_name = net.name;
    empty.is_3d = true;
    empty.root_tree_index = 0;

    if (net.pins.size() <= 1) {
        empty.success = true;
        if (!net.pins.empty()) {
            TreeNodeState root;
            root.pin_index = 0;
            root.point = pinToPoint(net.pins[0]);
            root.node_type = TreeNodeState::NodeType::kPin;
            empty.tree_nodes.push_back(root);
        }
        (void) annotateDelayPublic(net, empty);
        return empty;
    }

    const int src = chooseSourcePin(net);
    if (src < 0 || src >= static_cast<int>(net.pins.size())) {
        return empty;
    }

    PartialRouteState seed;
    seed.result.net_name = net.name;
    seed.result.is_3d = true;
    seed.result.root_tree_index = 0;
    TreeNodeState root;
    root.pin_index = src;
    root.point = pinToPoint(net.pins[src]);
    root.node_type = TreeNodeState::NodeType::kPin;
    seed.result.tree_nodes.push_back(root);
    seed.in_tree.assign(net.pins.size(), false);
    seed.in_tree[src] = true;
    seed.attached_sinks = 0;
    seed.score.valid = true;

    std::vector<PartialRouteState> beam{seed};
    const std::vector<int> sink_order = build3DSinkOrder(net, src);
    const int beam_width = std::max(1, params_.beam_width_3d);

    for (int sink_pin_index : sink_order) {
        std::vector<PartialRouteState> expanded;
        for (const PartialRouteState& st : beam) {
            if (sink_pin_index < 0 || sink_pin_index >= static_cast<int>(st.in_tree.size()) || st.in_tree[sink_pin_index]) {
                expanded.push_back(st);
                continue;
            }

            const DieId sink_die = normalizeDie(pinToPoint(net.pins[sink_pin_index]).die);
            const DieId src_die = normalizeDie(seed.result.tree_nodes[seed.result.root_tree_index].point.die);
            std::vector<PartialRouteState> local;
            if (sink_die == src_die) {
                local = expandSinkSameDieCandidates(net, sink_pin_index, st);
            } else {
                local = expandSinkCrossDieCandidates(net, sink_pin_index, st);
                auto same = expandSinkSameDieCandidates(net, sink_pin_index, st);
                local.insert(local.end(), same.begin(), same.end());
            }
            expanded.insert(expanded.end(), local.begin(), local.end());
        }

        std::sort(expanded.begin(), expanded.end(), [&](const PartialRouteState& a, const PartialRouteState& b) {
            return isBetterScore(a.score, b.score);
        });
        if (static_cast<int>(expanded.size()) > beam_width) {
            expanded.resize(beam_width);
        }
        if (expanded.empty()) {
            if (params_.verbose) {
                std::cerr << "[PDTreeRouter][3D][beam-fail] net=" << net.name
                          << " sink_pin_index=" << sink_pin_index
                          << " reason=expanded_empty_try_robust_fallback" << std::endl;
            }
            NetRouteResult fallback = route3DNetRobustFallback(net, "expanded_empty");
            if (fallback.success) {
                std::unordered_set<int> fallback_hbts;
                collectHBTsFromSegments(fallback.segments, fallback_hbts);
                commitNetLevelHBTReservation(fallback_hbts, net.name);
                return fallback;
            }
            return empty;
        }
        beam = std::move(expanded);
    }

    if (beam.empty()) {
        NetRouteResult fallback = route3DNetRobustFallback(net, "beam_empty");
        if (fallback.success) {
            std::unordered_set<int> fallback_hbts;
            collectHBTsFromSegments(fallback.segments, fallback_hbts);
            commitNetLevelHBTReservation(fallback_hbts, net.name);
            return fallback;
        }
        return empty;
    }

    std::sort(beam.begin(), beam.end(), [&](const PartialRouteState& a, const PartialRouteState& b) {
        return isBetterScore(a.score, b.score);
    });
    PartialRouteState best = beam.front();
    if (!best.score.valid) {
        NetRouteResult fallback = route3DNetRobustFallback(net, "best_invalid");
        if (fallback.success) {
            std::unordered_set<int> fallback_hbts;
            collectHBTsFromSegments(fallback.segments, fallback_hbts);
            commitNetLevelHBTReservation(fallback_hbts, net.name);
            return fallback;
        }
        return empty;
    }
    best.result.success = true;
    best.result.status = "ok";
    best.result.validation = validateRouteResultTopology(best.result);
    if (!best.result.validation.valid) {
        best.result.success = false;
        best.result.status = "invalid_topology";
        for (const auto& err : best.result.validation.errors) {
            if (!best.result.fail_reason.empty()) {
                best.result.fail_reason += " | ";
            }
            best.result.fail_reason += err;
        }
        best.result.delay_summary.ready = false;
        return best.result;
    }

    best.result.route_cost_total = best.accumulated_cost;
    (void) annotateDelayPublic(net, best.result);
    commitNetLevelHBTReservation(best.local_reserved_hbts, net.name);
    return best.result;
}

NetRouteResult PDTreeRouter::route3DNetRobustFallback(const Net& net,
                                                       const std::string& reason)
{
    NetRouteResult result;
    result.net_name = net.name;
    result.is_3d = true;
    result.root_tree_index = 0;

    std::ostringstream dbg;
    dbg << "[3d_fallback] net=" << net.name
        << " pin_count=" << net.pins.size()
        << " reason=" << reason;

    const int src = chooseSourcePin(net);
    if (src < 0 || src >= static_cast<int>(net.pins.size())) {
        dbg << " fallback_success=false failure=invalid_source";
        appendPDRouteDebugLine(dbg.str());
        return result;
    }

    TreeNodeState root;
    root.pin_index = src;
    root.point = pinToPoint(net.pins[src]);
    root.node_type = TreeNodeState::NodeType::kPin;
    result.tree_nodes.push_back(root);

    std::vector<int> top_pin_indices;
    std::vector<int> bottom_pin_indices;
    top_pin_indices.reserve(net.pins.size());
    bottom_pin_indices.reserve(net.pins.size());
    for (int i = 0; i < static_cast<int>(net.pins.size()); ++i) {
        const DieId d = normalizeDie(pinToPoint(net.pins[i]).die);
        if (d == DieId::kTop) {
            top_pin_indices.push_back(i);
        } else {
            bottom_pin_indices.push_back(i);
        }
    }

    dbg << " source_pin_index=" << src
        << " source_pin_name=" << net.pins[src].name
        << " source_die=" << dieToString(root.point.die)
        << " top_pin_count=" << top_pin_indices.size()
        << " bottom_pin_count=" << bottom_pin_indices.size();

    if (top_pin_indices.empty() || bottom_pin_indices.empty()) {
        NetRouteResult same_die = route2DNet(net);
        dbg << " fallback_success=" << (same_die.success ? "true" : "false")
            << " fallback_mode=route2D";
        appendPDRouteDebugLine(dbg.str());
        return same_die;
    }

    const int hbt_id = selectRobustFallbackHBT(net, top_pin_indices, bottom_pin_indices);
    if (hbt_id < 0) {
        dbg << " fallback_selected_hbt_id=-1 fallback_success=false failure=no_hbt";
        appendPDRouteDebugLine(dbg.str());
        return result;
    }

    const auto& hs = grid_.hbt.getSlots().at(hbt_id);
    const DieId source_die = normalizeDie(root.point.die);
    const DieId opposite_die = (source_die == DieId::kTop) ? DieId::kBottom : DieId::kTop;
    const std::vector<int>& opposite_pins = (opposite_die == DieId::kTop) ? top_pin_indices : bottom_pin_indices;

    int anchor_pin = -1;
    int best_anchor_score = std::numeric_limits<int>::max();
    for (int pin_index : opposite_pins) {
        const RoutedPoint sink_pt = pinToPoint(net.pins[pin_index]);
        const int score = manhattan(hs.x, hs.y, sink_pt.x, sink_pt.y) + manhattan(root.point, sink_pt);
        if (score < best_anchor_score) {
            best_anchor_score = score;
            anchor_pin = pin_index;
        }
    }
    if (anchor_pin < 0) {
        dbg << " fallback_selected_hbt_id=" << hbt_id
            << " fallback_success=false failure=no_opposite_anchor";
        appendPDRouteDebugLine(dbg.str());
        return result;
    }

    int parent_idx = -1;
    int best_parent_dist = std::numeric_limits<int>::max();
    for (int i = 0; i < static_cast<int>(result.tree_nodes.size()); ++i) {
        if (normalizeDie(result.tree_nodes[i].point.die) != source_die) {
            continue;
        }
        const int d = manhattan(result.tree_nodes[i].point.x, result.tree_nodes[i].point.y, hs.x, hs.y);
        if (d < best_parent_dist) {
            best_parent_dist = d;
            parent_idx = i;
        }
    }
    if (parent_idx < 0) {
        dbg << " fallback_selected_hbt_id=" << hbt_id
            << " fallback_success=false failure=no_parent_source_die";
        appendPDRouteDebugLine(dbg.str());
        return result;
    }

    const RoutedPoint parent_pt = result.tree_nodes[parent_idx].point;
    const RoutedPoint h_source{hs.x, hs.y, source_die};
    const RoutedPoint h_opp{hs.x, hs.y, opposite_die};
    const RoutedPoint sink_pt = pinToPoint(net.pins[anchor_pin]);

    std::vector<RoutedSegment> seg_parent_to_hbt;
    std::vector<RoutedSegment> seg_hbt_to_sink;
    if (!build2DManhattanConnection(parent_pt, h_source, seg_parent_to_hbt)
        || !build2DManhattanConnection(h_opp, sink_pt, seg_hbt_to_sink)) {
        dbg << " fallback_selected_hbt_id=" << hbt_id
            << " fallback_success=false failure=manhattan_failed";
        appendPDRouteDebugLine(dbg.str());
        return result;
    }

    if (!commit3DBranchWithHBTNode(result,
                                   anchor_pin,
                                   parent_idx,
                                   -1,
                                   seg_parent_to_hbt,
                                   seg_hbt_to_sink,
                                   h_source,
                                   sink_pt,
                                   std::vector<int>{hbt_id},
                                   source_die,
                                   opposite_die,
                                   hbt_id)) {
        dbg << " fallback_selected_hbt_id=" << hbt_id
            << " fallback_success=false failure=commit_3d_branch_failed";
        appendPDRouteDebugLine(dbg.str());
        return result;
    }

    std::vector<bool> in_tree(net.pins.size(), false);
    in_tree[src] = true;
    in_tree[anchor_pin] = true;
    if (!connectRemainingPinsSameDieDeterministic(net, result, in_tree)) {
        dbg << " fallback_selected_hbt_id=" << hbt_id
            << " fallback_success=false failure=connect_remaining_failed";
        appendPDRouteDebugLine(dbg.str());
        NetRouteResult failed;
        failed.net_name = net.name;
        failed.is_3d = true;
        failed.root_tree_index = 0;
        return failed;
    }

    result.success = true;
    result.status = "ok";
    result.validation = validateRouteResultTopology(result);
    (void) annotateDelayPublic(net, result);
    int hbt_segments = 0;
    for (const RoutedSegment& s : result.segments) {
        if (s.uses_hbt) {
            ++hbt_segments;
        }
    }
    int connected_count = 0;
    for (bool in : in_tree) {
        connected_count += in ? 1 : 0;
    }
    dbg << " fallback_selected_hbt_id=" << hbt_id
        << " fallback_hbt_xy=(" << hs.x << "," << hs.y << ")"
        << " opposite_anchor_pin_index=" << anchor_pin
        << " opposite_anchor_pin_name=" << net.pins[anchor_pin].name
        << " remaining_pins_connected_count=" << connected_count
        << " fallback_success=true"
        << " final_tree_nodes=" << result.tree_nodes.size()
        << " final_segments=" << result.segments.size()
        << " hbt_segments=" << hbt_segments;
    appendPDRouteDebugLine(dbg.str());
    return result;
}

std::vector<int> PDTreeRouter::build3DSinkOrder(const Net& net, int source_pin_index) const
{
    std::vector<int> sinks;
    const RoutedPoint src_pt = pinToPoint(net.pins[source_pin_index]);
    for (int i = 0; i < static_cast<int>(net.pins.size()); ++i) {
        if (i != source_pin_index) {
            sinks.push_back(i);
        }
    }

    // 3D attach order is intentionally not original pin order:
    // cross-die + far + high-cap sinks are prioritized so that early HBT decisions
    // shape subtree topology instead of only one local parent-sink pair.
    std::sort(sinks.begin(), sinks.end(), [&](int a, int b) {
        const RoutedPoint pa = pinToPoint(net.pins[a]);
        const RoutedPoint pb = pinToPoint(net.pins[b]);
        const bool a_cross = normalizeDie(pa.die) != normalizeDie(src_pt.die);
        const bool b_cross = normalizeDie(pb.die) != normalizeDie(src_pt.die);
        if (a_cross != b_cross) {
            return a_cross;
        }
        const int da = manhattan(pa, src_pt);
        const int db = manhattan(pb, src_pt);
        if (da != db) {
            return da > db;
        }
        const double ca = net.pins[a].has_input_cap ? net.pins[a].input_cap : params_.default_sink_cap;
        const double cb = net.pins[b].has_input_cap ? net.pins[b].input_cap : params_.default_sink_cap;
        return ca > cb;
    });
    return sinks;
}

std::vector<PDTreeRouter::PartialRouteState> PDTreeRouter::expandSinkSameDieCandidates(
    const Net& net,
    int sink_pin_index,
    const PartialRouteState& state) const
{
    std::vector<PartialRouteState> out;
    const Pin& sink = net.pins[sink_pin_index];
    const RoutedPoint sink_pt = pinToPoint(sink);
    const auto parent_candidates = collectCandidateParents(net, state.result, sink);

    for (int tidx : parent_candidates) {
        const RoutedPoint parent_pt = state.result.tree_nodes[tidx].point;
        if (normalizeDie(parent_pt.die) != normalizeDie(sink_pt.die)) {
            continue;
        }

        std::vector<RoutedSegment> segs;
        if (!build2DManhattanConnection(parent_pt, sink_pt, segs)) {
            continue;
        }

        const CandidateScore candidate_score = scoreAttachmentCandidate(
            net,
            state.result,
            segs,
            sink_pin_index,
            tidx,
            sink_pt,
            0);

        if (!candidate_score.valid) {
            continue;
        }

        PartialRouteState next = state;
        commitSegments(next.result, segs, sink_pin_index, tidx, sink_pt, 0);
        next.in_tree[sink_pin_index] = true;
        next.attached_sinks += 1;

        // Beam search must use accumulated report-aligned PD cost.
        // Do not replace this with EDCompute or total-wirelength state scoring.
        next.accumulated_cost = state.accumulated_cost + candidate_score.objective;
        next.score = candidate_score;
        next.score.objective = next.accumulated_cost;
        next.score.hbt_count = countHBTSegments(next.result);
        next.score.total_wirelength = totalWireLength(next.result.segments);

        out.push_back(std::move(next));
        if (static_cast<int>(out.size()) >= std::max(1, params_.beam_branch_candidates_3d)) {
            break;
        }
    }
    return out;
}

std::vector<PDTreeRouter::PartialRouteState> PDTreeRouter::expandSinkCrossDieCandidates(
    const Net& net,
    int sink_pin_index,
    const PartialRouteState& state) const
{
    std::vector<PartialRouteState> out;
    const Pin& sink = net.pins[sink_pin_index];
    const RoutedPoint sink_pt = pinToPoint(sink);
    const auto parent_candidates = collectCandidateParents(net, state.result, sink);

    for (int tidx : parent_candidates) {
        const RoutedPoint parent_pt = state.result.tree_nodes[tidx].point;
        const DieId parent_die = normalizeDie(parent_pt.die);
        const DieId sink_die = normalizeDie(sink_pt.die);

        if (parent_die == sink_die) {
            continue;
        }

        const std::vector<RoutedPoint> subtree_anchor{parent_pt, sink_pt};
        const auto hbts = collectCandidateHBTsForSubtree(
            net,
            subtree_anchor,
            state.local_reserved_hbts);

        if (hbts.empty()) {
            continue;
        }

        int emitted = 0;
        for (int hid : hbts) {
            if (hid < 0 || hid >= static_cast<int>(grid_.hbt.getSlots().size())) {
                continue;
            }

            const auto& slot = grid_.hbt.getSlots().at(hid);
            const RoutedPoint hbt_from{slot.x, slot.y, parent_die};
            const RoutedPoint hbt_to{slot.x, slot.y, sink_die};
            const double hbt_to_sink_length =
                static_cast<double>(manhattan(hbt_to, sink_pt));

            const CandidateScore candidate_score = scoreAttachmentCandidate(
                net,
                state.result,
                {},
                sink_pin_index,
                tidx,
                hbt_from,
                1,
                hbt_to_sink_length,
                hid);

            if (!candidate_score.valid) {
                continue;
            }

            PartialRouteState next = state;
            if (!commitCrossDieBranch(next.result, tidx, sink_pin_index, hid)) {
                continue;
            }

            next.local_reserved_hbts.insert(hid);
            next.in_tree[sink_pin_index] = true;
            next.attached_sinks += 1;

            if (params_.enable_hbt_inner_node_optimization) {
                (void) optimizeHBTInnerNodesForState(net, next);
            }

            // Beam search must use accumulated report-aligned PD cost.
            next.accumulated_cost = state.accumulated_cost + candidate_score.objective;
            next.score = candidate_score;
            next.score.objective = next.accumulated_cost;
            next.score.hbt_count = countHBTSegments(next.result);
            next.score.total_wirelength = totalWireLength(next.result.segments);

            out.push_back(std::move(next));
            ++emitted;
            if (emitted >= std::max(1, params_.beam_branch_candidates_3d)) {
                break;
            }
        }
    }
    return out;
}

int PDTreeRouter::selectRobustFallbackHBT(const Net& net,
                                          const std::vector<int>& top_pin_indices,
                                          const std::vector<int>& bottom_pin_indices) const
{
    const auto& slots = grid_.hbt.getSlots();
    std::unordered_set<int> empty_local_reserved;
    long long best_score = std::numeric_limits<long long>::max();
    int best_hbt = -1;

    for (int hid = 0; hid < static_cast<int>(slots.size()); ++hid) {
        if (!isHBTAvailableForNet(hid, net.name, empty_local_reserved)) {
            continue;
        }
        const auto& slot = slots[hid];
        int tix = -1;
        int tiy = -1;
        int bix = -1;
        int biy = -1;
        if (grid_.top.locateCell(slot.x, slot.y, tix, tiy) < 0
            || grid_.bottom.locateCell(slot.x, slot.y, bix, biy) < 0) {
            continue;
        }

        long long score = 0;
        for (int pin_idx : top_pin_indices) {
            const RoutedPoint p = pinToPoint(net.pins[pin_idx]);
            score += static_cast<long long>(manhattan(slot.x, slot.y, p.x, p.y));
        }
        for (int pin_idx : bottom_pin_indices) {
            const RoutedPoint p = pinToPoint(net.pins[pin_idx]);
            score += static_cast<long long>(manhattan(slot.x, slot.y, p.x, p.y));
        }
        if (hbt_used_[hid]) {
            score += 1000000LL;
        }

        if (score < best_score) {
            best_score = score;
            best_hbt = hid;
        }
    }
    return best_hbt;
}

bool PDTreeRouter::connectRemainingPinsSameDieDeterministic(const Net& net,
                                                             NetRouteResult& result,
                                                             std::vector<bool>& in_tree) const
{
    for (int pin_index = 0; pin_index < static_cast<int>(net.pins.size()); ++pin_index) {
        if (pin_index < static_cast<int>(in_tree.size()) && in_tree[pin_index]) {
            continue;
        }
        const RoutedPoint sink_pt = pinToPoint(net.pins[pin_index]);
        const DieId sink_die = normalizeDie(sink_pt.die);

        int parent_idx = -1;
        int best_dist = std::numeric_limits<int>::max();
        for (int i = 0; i < static_cast<int>(result.tree_nodes.size()); ++i) {
            const RoutedPoint parent_pt = result.tree_nodes[i].point;
            if (normalizeDie(parent_pt.die) != sink_die) {
                continue;
            }
            const int dist = manhattan(parent_pt, sink_pt);
            if (dist < best_dist) {
                best_dist = dist;
                parent_idx = i;
            }
        }

        if (parent_idx < 0) {
            return false;
        }

        const RoutedPoint parent_pt = result.tree_nodes[parent_idx].point;
        std::vector<RoutedSegment> segs;
        if (!build2DManhattanConnection(parent_pt, sink_pt, segs)) {
            return false;
        }
        commitSegments(result, segs, pin_index, parent_idx, sink_pt, 0);
        if (pin_index < static_cast<int>(in_tree.size())) {
            in_tree[pin_index] = true;
        }
    }
    return true;
}

int PDTreeRouter::chooseSourcePin(const Net& net) const
{
    if (net.source_pin_index >= 0 && net.source_pin_index < static_cast<int>(net.pins.size())) {
        return net.source_pin_index;
    }

    // Future hook: if reliable pin direction metadata (output/input) becomes
    // available from netlist/liberty, prioritize output pins first.
    for (int i = 0; i < static_cast<int>(net.pins.size()); ++i) {
        if (!net.pins[i].is_bterm) {
            return i;
        }
    }
    return net.pins.empty() ? -1 : 0;
}

bool PDTreeRouter::connectSinkSameDie(const Net& net,
                                      int sink_pin_index,
                                      DieId net_route_die,
                                      NetRouteResult& result,
                                      std::vector<bool>& in_tree)
{
    const Pin& sink = net.pins[sink_pin_index];
    RoutedPoint sink_pt = pinToPoint(sink);
    sink_pt.die = net.is_3d ? normalizeDie(sink_pt.die)
                            : resolve2DPinDie(sink, net_route_die);

    const auto candidates = collectCandidateParents(net, result, sink);
    if (candidates.empty()) {
        return false;
    }

    CandidateScore best_score;
    int best_parent_tree_idx = -1;
    std::vector<RoutedSegment> best_segs;

    for (int tidx : candidates) {
        const TreeNodeState& parent = result.tree_nodes[tidx];
        RoutedPoint parent_pt = parent.point;
        parent_pt.die = net.is_3d ? normalizeDie(parent.point.die)
                                  : resolve2DPointDie(parent.point, net_route_die);
        const DieId parent_die_resolved = parent_pt.die;
        const DieId sink_die_resolved = sink_pt.die;
        if (parent_die_resolved != sink_die_resolved) {
            if (params_.verbose) {
                std::cerr << "[PDTreeRouter][same-die-skip] net=" << net.name
                          << " sink_pin_index=" << sink_pin_index
                          << " sink_pin=" << sink.name
                          << " parent_die=" << (parent_die_resolved == DieId::kBottom ? "Bottom" : "Top")
                          << " sink_die=" << (sink_die_resolved == DieId::kBottom ? "Bottom" : "Top")
                          << " reason=die_mismatch" << std::endl;
            }
            continue;
        }

        int p_ix = -1;
        int p_iy = -1;
        int s_ix = -1;
        int s_iy = -1;
        int p_cid = -1;
        int s_cid = -1;
        if (sink_die_resolved == DieId::kTop) {
            p_cid = grid_.top.locateCell(parent_pt.x, parent_pt.y, p_ix, p_iy);
            s_cid = grid_.top.locateCell(sink_pt.x, sink_pt.y, s_ix, s_iy);
        } else {
            p_cid = grid_.bottom.locateCell(parent_pt.x, parent_pt.y, p_ix, p_iy);
            s_cid = grid_.bottom.locateCell(sink_pt.x, sink_pt.y, s_ix, s_iy);
        }
        if (p_cid < 0 || s_cid < 0) {
            continue;
        }

        std::vector<RoutedSegment> segs;
        if (!build2DManhattanConnection(parent_pt, sink_pt, segs)) {
            continue;
        }

        const CandidateScore score = scoreAttachmentCandidate(net,
                                                              result,
                                                              segs,
                                                              sink_pin_index,
                                                              tidx,
                                                              sink_pt,
                                                              0);
        if (isBetterScore(score, best_score)) {
            best_score = score;
            best_parent_tree_idx = tidx;
            best_segs = std::move(segs);
        }
    }

    if (best_parent_tree_idx < 0) {
        return false;
    }

    commitSegments(result,
                   best_segs,
                   sink_pin_index,
                   best_parent_tree_idx,
                   sink_pt,
                   0);
    result.route_cost_total += best_score.objective;
    result.route_wire_delay += best_score.cost.wire_delay;
    result.route_parent_load_delay += best_score.cost.parent_load_delay;
    result.route_hbt_rc_delay += best_score.cost.hbt_rc_delay;
    result.route_hbt_net_penalty_delay += best_score.cost.hbt_net_penalty_delay;
    result.route_hbt_net_quad_penalty_delay += best_score.cost.hbt_net_quad_penalty_delay;
    result.route_hbt_path_penalty_delay += best_score.cost.hbt_path_penalty_delay;
    result.route_stretch_penalty_delay += best_score.cost.stretch_penalty_delay;
    in_tree[sink_pin_index] = true;
    return true;
}

bool PDTreeRouter::connectSinkCrossDie(const Net& net,
                                       int sink_pin_index,
                                       NetRouteResult& result,
                                       std::vector<bool>& in_tree,
                                       std::unordered_set<int>& local_reserved_hbts)
{
    const Pin& sink = net.pins[sink_pin_index];
    const RoutedPoint sink_pt = pinToPoint(sink);

    const auto parent_candidates = collectCandidateParents(net, result, sink);
    if (parent_candidates.empty()) {
        return false;
    }

    CandidateScore best_score;
    int best_parent_tree_idx = -1;
    int best_hbt_id = -1;
    std::vector<RoutedSegment> best_segs;

    for (int tidx : parent_candidates) {
        const TreeNodeState& parent = result.tree_nodes[tidx];
        if (normalizeDie(parent.point.die) == normalizeDie(sink_pt.die)) {
            continue;
        }

        Pin pseudo_parent_pin;
        pseudo_parent_pin.x = parent.point.x;
        pseudo_parent_pin.y = parent.point.y;
        pseudo_parent_pin.die = parent.point.die;

        const auto hbts = collectCandidateHBTs(net, pseudo_parent_pin, sink, local_reserved_hbts);
        for (int hid : hbts) {
            const auto& hs = grid_.hbt.getSlots().at(hid);
            int tix = -1;
            int tiy = -1;
            int bix = -1;
            int biy = -1;
            if (grid_.top.locateCell(hs.x, hs.y, tix, tiy) < 0
                || grid_.bottom.locateCell(hs.x, hs.y, bix, biy) < 0) {
                continue;
            }

            std::vector<RoutedSegment> segs;
            if (!build3DConnectionViaHBT(parent.point, sink, hid, segs)) {
                continue;
            }

            const RoutedPoint hbt_from{hs.x, hs.y, normalizeDie(parent.point.die)};
            const RoutedPoint hbt_to{hs.x, hs.y, normalizeDie(sink_pt.die)};
            const double hbt_to_sink_length =
                static_cast<double>(manhattan(hbt_to, sink_pt));

            // Score cross-die candidate by the report-aligned local PD cost.
            // attach_point is the source-side HBT endpoint; the HBT-to-sink
            // segment is included through extra_path_after_attach.
            const CandidateScore score = scoreAttachmentCandidate(net,
                                                                  result,
                                                                  segs,
                                                                  sink_pin_index,
                                                                  tidx,
                                                                  hbt_from,
                                                                  1,
                                                                  hbt_to_sink_length,
                                                                  hid);
            if (isBetterScore(score, best_score)) {
                best_score = score;
                best_parent_tree_idx = tidx;
                best_hbt_id = hid;
                best_segs = std::move(segs);
            }
        }
    }

    if (best_parent_tree_idx < 0 || best_hbt_id < 0) {
        return false;
    }

    commitSegments(result,
                   best_segs,
                   sink_pin_index,
                   best_parent_tree_idx,
                   sink_pt,
                   1);
    result.route_cost_total += best_score.objective;
    result.route_wire_delay += best_score.cost.wire_delay;
    result.route_parent_load_delay += best_score.cost.parent_load_delay;
    result.route_hbt_rc_delay += best_score.cost.hbt_rc_delay;
    result.route_hbt_net_penalty_delay += best_score.cost.hbt_net_penalty_delay;
    result.route_hbt_net_quad_penalty_delay += best_score.cost.hbt_net_quad_penalty_delay;
    result.route_hbt_path_penalty_delay += best_score.cost.hbt_path_penalty_delay;
    result.route_stretch_penalty_delay += best_score.cost.stretch_penalty_delay;

    collectHBTsFromSegments(best_segs, local_reserved_hbts);
    in_tree[sink_pin_index] = true;
    return true;
}

PDTreeRouter::ReportCostBreakdown PDTreeRouter::computeReportPDCost(
    const Net& net,
    const NetRouteResult& current_tree,
    int parent_tree_index,
    int sink_pin_index,
    const RoutedPoint& attach_point,
    bool introduces_hbt,
    int hbt_id,
    double extra_path_after_attach_dbu) const
{
    ReportCostBreakdown cost;

    if (parent_tree_index < 0
        || parent_tree_index >= static_cast<int>(current_tree.tree_nodes.size())) {
        return cost;
    }

    if (sink_pin_index < 0 || sink_pin_index >= static_cast<int>(net.pins.size())) {
        return cost;
    }

    if (current_tree.root_tree_index < 0
        || current_tree.root_tree_index >= static_cast<int>(current_tree.tree_nodes.size())) {
        return cost;
    }

    const TreeNodeState& parent = current_tree.tree_nodes[parent_tree_index];
    const Pin& sink_pin = net.pins[sink_pin_index];
    const RoutedPoint root_point = current_tree.tree_nodes[current_tree.root_tree_index].point;
    const RoutedPoint sink_point = pinToPoint(sink_pin);

    cost.parent_to_attach_dbu = static_cast<double>(manhattan(parent.point, attach_point));
    cost.attach_to_sink_dbu = std::max(0.0, extra_path_after_attach_dbu);
    cost.branch_wire_dbu = cost.parent_to_attach_dbu + cost.attach_to_sink_dbu;
    cost.parent_to_attach_um = db_.dbuToMicronLength(static_cast<int>(cost.parent_to_attach_dbu));
    cost.attach_to_sink_um = db_.dbuToMicronLength(static_cast<int>(cost.attach_to_sink_dbu));
    cost.branch_wire_um = cost.parent_to_attach_um + cost.attach_to_sink_um;
    cost.parent_path_res = parent.path_res_from_root;
    cost.parent_path_cap = parent.path_cap_from_root;
    cost.parent_path_delay = parent.path_delay_from_root;
    cost.sink_cap = sink_pin.has_input_cap
                        ? sink_pin.input_cap
                        : params_.default_sink_cap;
    const EffectiveRC rc1 = selectWireRCForSegment(parent.point.die, parent.point, attach_point);
    const EffectiveRC rc2 = selectWireRCForSegment(sink_point.die, attach_point, sink_point);
    cost.parent_segment_die = dieToString(parent.point.die);
    cost.attach_segment_die = dieToString(sink_point.die);
    cost.parent_segment_rc_res_per_um = rc1.unit_res;
    cost.parent_segment_rc_cap_per_um = rc1.unit_cap;
    cost.attach_segment_rc_res_per_um = rc2.unit_res;
    cost.attach_segment_rc_cap_per_um = rc2.unit_cap;
    cost.wire_res_parent_to_attach = rc1.unit_res * cost.parent_to_attach_um;
    cost.wire_cap_parent_to_attach = rc1.unit_cap * cost.parent_to_attach_um;
    cost.wire_res_attach_to_sink = rc2.unit_res * cost.attach_to_sink_um;
    cost.wire_cap_attach_to_sink = rc2.unit_cap * cost.attach_to_sink_um;

    if (introduces_hbt) {
        if (hbt_id < 0 || hbt_id >= static_cast<int>(grid_.hbt.getSlots().size())) {
            return cost;
        }

        cost.current_net_hbt_count = countHBTSegments(current_tree);
        cost.net_hbt_count_after = cost.current_net_hbt_count + 1;
        cost.path_hbt_count_after = parent.hbt_count_from_root + 1;
        if (params_.report_cost.max_hbt_per_path > 0
            && cost.path_hbt_count_after > params_.report_cost.max_hbt_per_path) {
            return cost;
        }
        if (params_.report_cost.max_hbt_per_net > 0
            && cost.current_net_hbt_count >= params_.report_cost.max_hbt_per_net) {
            return cost;
        }
        cost.hbt_res = params_.hbt_res;
        cost.hbt_cap = params_.hbt_cap;
        cost.parent_load_delay = cost.parent_path_res * (cost.wire_cap_parent_to_attach + cost.hbt_cap + cost.wire_cap_attach_to_sink + cost.sink_cap);
        cost.wire_delay = 0.5 * cost.wire_res_parent_to_attach * cost.wire_cap_parent_to_attach
                        + cost.wire_res_parent_to_attach * (cost.hbt_cap + cost.wire_cap_attach_to_sink + cost.sink_cap)
                        + 0.5 * cost.wire_res_attach_to_sink * cost.wire_cap_attach_to_sink
                        + cost.wire_res_attach_to_sink * cost.sink_cap;
        cost.hbt_rc_delay = cost.hbt_res * (cost.wire_cap_attach_to_sink + cost.sink_cap) + 0.5 * cost.hbt_res * cost.hbt_cap;
        cost.hbt_unit_delay = cost.hbt_res * (cost.hbt_cap + cost.sink_cap);
        if (cost.hbt_unit_delay < 1e-12) cost.hbt_unit_delay = params_.report_cost.hbt_unit_delay_scale;
        cost.hbt_net_penalty_delay = cost.hbt_unit_delay * params_.report_cost.hbt_net_penalty_scale * cost.net_hbt_count_after;
        cost.hbt_net_quad_penalty_delay = cost.hbt_unit_delay * params_.report_cost.hbt_net_quad_penalty_scale * cost.net_hbt_count_after * cost.net_hbt_count_after;
        cost.hbt_path_penalty_delay = cost.hbt_unit_delay * params_.report_cost.hbt_path_penalty_scale * cost.path_hbt_count_after * cost.path_hbt_count_after;
    } else {
        cost.parent_load_delay = cost.parent_path_res * (cost.wire_cap_parent_to_attach + cost.sink_cap);
        cost.wire_delay = 0.5 * cost.wire_res_parent_to_attach * cost.wire_cap_parent_to_attach
                        + cost.wire_res_parent_to_attach * cost.sink_cap;
    }
    const double source_to_sink_dbu = std::max(1.0, static_cast<double>(manhattan(root_point, sink_point)));
    const double candidate_path_dbu = static_cast<double>(parent.path_length_from_root) + cost.branch_wire_dbu;
    const double stretch_ratio = candidate_path_dbu / source_to_sink_dbu;
    if (stretch_ratio > params_.report_cost.stretch_limit) {
        cost.stretch_penalty_delay = (stretch_ratio - params_.report_cost.stretch_limit)
                                   * (cost.wire_delay + cost.parent_load_delay + cost.hbt_rc_delay);
    }
    cost.weighted_wire_delay = params_.report_cost.coef_wire_delay * cost.wire_delay;
    cost.weighted_parent_load_delay = params_.report_cost.coef_parent_load_delay * cost.parent_load_delay;
    cost.weighted_hbt_rc_delay = params_.report_cost.coef_hbt_rc_delay * cost.hbt_rc_delay;
    cost.weighted_hbt_net_penalty = params_.report_cost.coef_hbt_net_penalty * cost.hbt_net_penalty_delay;
    cost.weighted_hbt_net_quad_penalty = params_.report_cost.coef_hbt_net_quad_penalty * cost.hbt_net_quad_penalty_delay;
    cost.weighted_hbt_path_penalty = params_.report_cost.coef_hbt_path_penalty * cost.hbt_path_penalty_delay;
    cost.weighted_stretch_penalty = params_.report_cost.coef_stretch_penalty * cost.stretch_penalty_delay;
    cost.total = cost.weighted_wire_delay + cost.weighted_parent_load_delay + cost.weighted_hbt_rc_delay
               + cost.weighted_hbt_net_penalty + cost.weighted_hbt_net_quad_penalty
               + cost.weighted_hbt_path_penalty + cost.weighted_stretch_penalty;

    cost.valid = true;
    return cost;
}

PDTreeRouter::CandidateScore PDTreeRouter::scoreAttachmentCandidate(
    const Net& net,
    const NetRouteResult& current,
    const std::vector<RoutedSegment>& candidate_segs,
    int sink_pin_index,
    int parent_tree_index,
    const RoutedPoint& sink_attach_point,
    int extra_hbt_count,
    double extra_path_after_attach,
    int hbt_id) const
{
    CandidateScore score;
    const bool introduces_hbt = extra_hbt_count > 0;
    if (params_.cost_mode == CostMode::kTraditionalPDTree) {
        return scoreTraditionalCandidate(net, current, parent_tree_index, sink_pin_index, sink_attach_point, introduces_hbt, hbt_id, extra_path_after_attach);
    }

    score.cost = computeReportPDCost(
        net,
        current,
        parent_tree_index,
        sink_pin_index,
        sink_attach_point,
        introduces_hbt,
        hbt_id,
        extra_path_after_attach);

    if (!score.cost.valid) {
        return score;
    }

    score.valid = true;
    score.objective = score.cost.total;
    score.hbt_count = countHBTSegments(current) + extra_hbt_count;
    score.total_wirelength = totalWireLength(current.segments)
                           + totalWireLength(candidate_segs);

    if (params_.dump_candidate_cost_debug) {
        std::ofstream ofs("candidate_cost_debug.csv", std::ios::app);
        if (ofs) {
            ofs << net.name << ','
                << costModeToString(params_.cost_mode) << ','
                << (net.is_3d ? "3D" : "2D") << ','
                << sink_pin_index << ','
                << parent_tree_index << ','
                << introduces_hbt << ','
                << hbt_id << ','
                << score.cost.total << ','
                << score.cost.parent_to_attach_dbu << ','
                << score.cost.attach_to_sink_dbu << ','
                << score.cost.branch_wire_dbu << ','
                << score.cost.parent_to_attach_um << ','
                << score.cost.attach_to_sink_um << ','
                << score.cost.branch_wire_um << ','
                << score.cost.parent_path_res << ','
                << score.cost.parent_path_cap << ','
                << score.cost.sink_cap << ','
                << score.cost.wire_res_parent_to_attach << ','
                << score.cost.wire_cap_parent_to_attach << ','
                << score.cost.wire_res_attach_to_sink << ','
                << score.cost.wire_cap_attach_to_sink << ','
                << score.cost.parent_segment_die << ','
                << score.cost.attach_segment_die << ','
                << score.cost.parent_segment_rc_res_per_um << ','
                << score.cost.parent_segment_rc_cap_per_um << ','
                << score.cost.attach_segment_rc_res_per_um << ','
                << score.cost.attach_segment_rc_cap_per_um << ','
                << score.cost.wire_delay << ','
                << score.cost.parent_load_delay << ','
                << score.cost.hbt_rc_delay << ','
                << score.cost.hbt_net_penalty_delay << ','
                << score.cost.hbt_net_quad_penalty_delay << ','
                << score.cost.hbt_path_penalty_delay << ','
                << score.cost.stretch_penalty_delay << ','
                << score.cost.weighted_wire_delay << ','
                << score.cost.weighted_parent_load_delay << ','
                << score.cost.weighted_hbt_rc_delay << ','
                << score.cost.weighted_hbt_net_penalty << ','
                << score.cost.weighted_hbt_net_quad_penalty << ','
                << score.cost.weighted_hbt_path_penalty << ','
                << score.cost.weighted_stretch_penalty << ',' << score.cost.total << '\n';
        }
    }

    return score;
}

EffectiveRC PDTreeRouter::selectWireRCForSegment(DieId die,
                                                  const RoutedPoint& a,
                                                  const RoutedPoint& b) const
{
    EffectiveRC rc = db_.computeDirectionalRCForDie(die, isHorizontalPreferred(a, b));
    if (rc.unit_res <= 0.0 || rc.unit_cap <= 0.0) {
        rc.unit_res = params_.report_cost.default_wire_res_per_um;
        rc.unit_cap = params_.report_cost.default_wire_cap_per_um;
    }
    if (normalizeDie(die) == DieId::kTop) {
        rc.unit_res *= params_.top_wire_r_scale;
        rc.unit_cap *= params_.top_wire_c_scale;
    } else {
        rc.unit_res *= params_.bottom_wire_r_scale;
        rc.unit_cap *= params_.bottom_wire_c_scale;
    }
    return rc;
}

bool PDTreeRouter::isHorizontalPreferred(const RoutedPoint& a, const RoutedPoint& b) const
{
    return std::abs(a.x - b.x) >= std::abs(a.y - b.y);
}

PDTreeRouter::CandidateScore PDTreeRouter::scoreTraditionalCandidate(
    const Net& net,
    const NetRouteResult& current_tree,
    int parent_tree_index,
    int sink_pin_index,
    const RoutedPoint& attach_point,
    bool introduces_hbt,
    int hbt_id,
    double extra_path_after_attach_dbu) const
{
    CandidateScore score;
    if (parent_tree_index < 0 || parent_tree_index >= static_cast<int>(current_tree.tree_nodes.size())
        || sink_pin_index < 0 || sink_pin_index >= static_cast<int>(net.pins.size())) {
        return score;
    }
    const TreeNodeState& parent = current_tree.tree_nodes[parent_tree_index];
    const int current_hbt = countHBTSegments(current_tree);
    const int path_hbt_after = parent.hbt_count_from_root + (introduces_hbt ? 1 : 0);
    if (introduces_hbt) {
        if (hbt_id < 0 || current_hbt + 1 > params_.traditional_pdtree.max_hbt_per_net
            || path_hbt_after > params_.traditional_pdtree.max_hbt_per_path) {
            return score;
        }
    }
    const double dij = static_cast<double>(manhattan(parent.point, attach_point)) + std::max(0.0, extra_path_after_attach_dbu);
    const double li = static_cast<double>(parent.path_length_from_root);
    const double total = dij + params_.traditional_pdtree.alpha * li;
    score.valid = true;
    score.objective = total;
    score.cost.valid = true;
    score.cost.dij = dij;
    score.cost.li = li;
    score.cost.traditional_total = total;
    return score;
}

PDTreeRouter::CandidateScore PDTreeRouter::evaluateStateScore(
    const Net& /*net*/,
    const NetRouteResult& state_result) const
{
    CandidateScore score;

    // Deprecated compatibility function.
    // Core routing no longer calls EDCompute or the old global objective here.
    // The beam search path uses PartialRouteState::accumulated_cost.
    score.valid = true;
    score.objective = totalWireLength(state_result.segments);
    score.hbt_count = countHBTSegments(state_result);
    score.total_wirelength = score.objective;

    return score;
}

bool PDTreeRouter::isBetterScore(const CandidateScore& lhs,
                                 const CandidateScore& rhs) const
{
    if (!lhs.valid) {
        return false;
    }
    if (!rhs.valid) {
        return true;
    }

    constexpr double kEps = 1e-12;
    if (lhs.objective + kEps < rhs.objective) {
        return true;
    }
    if (rhs.objective + kEps < lhs.objective) {
        return false;
    }

    if (lhs.hbt_count != rhs.hbt_count) {
        return lhs.hbt_count < rhs.hbt_count;
    }

    if (lhs.total_wirelength + kEps < rhs.total_wirelength) {
        return true;
    }
    if (rhs.total_wirelength + kEps < lhs.total_wirelength) {
        return false;
    }

    return false;
}

PDTreeRouter::TimingSummary PDTreeRouter::evaluateTimingSummary(const Net& net,
                                                                const NetRouteResult& result) const
{
    TimingSummary t;

    if (result.tree_nodes.empty()
        || result.root_tree_index < 0
        || result.root_tree_index >= static_cast<int>(result.tree_nodes.size())) {
        return t;
    }

    EDCompute ed(db_, EDCompute::Params{params_.default_sink_cap,
                                        params_.source_res,
                                        params_.hbt_res,
                                        params_.hbt_cap,
                                        false});
    NetRouteResult copied = result;
    copied.success = true;
    (void) ed.annotateNetDelay(net, copied);

    if (copied.delay_summary.ready) {
        t.sink_count = static_cast<int>(copied.delay_summary.sink_delays.size());
        t.avg_sink_delay = copied.delay_summary.avg_sink_delay;
        t.max_sink_delay = copied.delay_summary.max_sink_delay;
    }

    const TreeNodeState& root = result.tree_nodes[result.root_tree_index];
    for (int i = 0; i < static_cast<int>(result.tree_nodes.size()); ++i) {
        const TreeNodeState& n = result.tree_nodes[i];
        if (i != result.root_tree_index) {
            t.total_wirelength += static_cast<double>(n.incoming_wire_length);
            t.hbt_count += n.incoming_hbt_count;
        }

        if (n.pin_index < 0 || n.pin_index >= static_cast<int>(net.pins.size())) {
            continue;
        }
        if (i == result.root_tree_index || n.pin_index == root.pin_index) {
            continue;
        }

        const int md = manhattan(root.point, n.point);
        const double stretch = std::max(0.0, static_cast<double>(n.path_length_from_root - md));
        t.avg_stretch += stretch;
    }

    if (t.sink_count > 0) {
        t.avg_stretch /= static_cast<double>(t.sink_count);
    }

    return t;
}

std::vector<int> PDTreeRouter::collectCandidateParents(const Net& /*net*/,
                                                       const NetRouteResult& result,
                                                       const Pin& sink) const
{
    std::vector<std::pair<int, int>> dist_idx;
    dist_idx.reserve(result.tree_nodes.size());

    for (int i = 0; i < static_cast<int>(result.tree_nodes.size()); ++i) {
        const auto& node = result.tree_nodes[i];
        if (node.node_type == TreeNodeState::NodeType::kHBT) {
            const auto& slots = grid_.hbt.getSlots();
            if (node.assigned_hbt_id < 0 || node.assigned_hbt_id >= static_cast<int>(slots.size())) {
                continue;
            }
            const auto& slot = slots[node.assigned_hbt_id];
            bool matched = false;
            for (const auto& seg : result.segments) {
                if (seg.uses_hbt && seg.hbt_id == node.assigned_hbt_id && seg.p1.x == slot.x && seg.p1.y == slot.y) { matched = true; break; }
            }
            if (!matched || node.point.x != slot.x || node.point.y != slot.y) {
                continue;
            }
        }
        const auto& p = node.point;
        const int d = manhattan(p.x, p.y, sink.x, sink.y);
        dist_idx.push_back({d, i});
    }

    std::sort(dist_idx.begin(), dist_idx.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    const int limit = std::max(1, params_.max_candidate_parents);
    std::vector<int> out;
    out.reserve(std::min(limit, static_cast<int>(dist_idx.size())));

    for (int i = 0; i < static_cast<int>(dist_idx.size()) && i < limit; ++i) {
        out.push_back(dist_idx[i].second);
    }
    return out;
}

std::vector<int> PDTreeRouter::collectCandidateHBTs(const Net& net,
                                                    const Pin& pin_a,
                                                    const Pin& pin_b,
                                                    const std::unordered_set<int>& local_reserved_hbts) const
{
    const int lx = std::min(pin_a.x, pin_b.x);
    const int ly = std::min(pin_a.y, pin_b.y);
    const int ux = std::max(pin_a.x, pin_b.x);
    const int uy = std::max(pin_a.y, pin_b.y);
    std::vector<RoutedPoint> subtree_points{
        RoutedPoint{pin_a.x, pin_a.y, pin_a.die},
        RoutedPoint{pin_b.x, pin_b.y, pin_b.die},
        RoutedPoint{(lx + ux) / 2, (ly + uy) / 2, DieId::kUnknown}};
    return collectCandidateHBTsForSubtree(net, subtree_points, local_reserved_hbts);
}

std::vector<int> PDTreeRouter::collectCandidateHBTsForSubtree(
    const Net& net,
    const std::vector<RoutedPoint>& subtree_points,
    const std::unordered_set<int>& local_reserved_hbts) const
{
    std::vector<int> out;
    if (subtree_points.empty() || grid_.hbt.getSlots().empty()) {
        return out;
    }
    int lx = std::numeric_limits<int>::max();
    int ly = std::numeric_limits<int>::max();
    int ux = std::numeric_limits<int>::min();
    int uy = std::numeric_limits<int>::min();
    int cx = 0;
    int cy = 0;
    for (const auto& p : subtree_points) {
        lx = std::min(lx, p.x);
        ly = std::min(ly, p.y);
        ux = std::max(ux, p.x);
        uy = std::max(uy, p.y);
        cx += p.x;
        cy += p.y;
    }
    cx /= static_cast<int>(subtree_points.size());
    cy /= static_cast<int>(subtree_points.size());
    const int auto_radius = std::max(1, std::min(db_.die_ux - db_.die_lx, db_.die_uy - db_.die_ly) / 16);
    const int radius = (params_.max_hbt_search_radius > 0) ? params_.max_hbt_search_radius : auto_radius;
    auto box_ids = grid_.hbt.querySlotsInBox(lx - radius, ly - radius, ux + radius, uy + radius);
    auto near_center = grid_.hbt.queryNearestSlots(cx, cy, std::max(1, params_.max_hbt_nearest_k));
    auto near_box = grid_.hbt.queryNearestSlotsToBox(lx, ly, ux, uy, std::max(1, params_.max_local_hbt_candidates));
    out.insert(out.end(), box_ids.begin(), box_ids.end());
    out.insert(out.end(), near_center.begin(), near_center.end());
    out.insert(out.end(), near_box.begin(), near_box.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    std::vector<int> filtered;
    for (int hid : out) {
        if (isHBTAvailableForNet(hid, net.name, local_reserved_hbts)) {
            filtered.push_back(hid);
        }
    }
    const int limit = std::max(1, params_.max_candidate_hbts);
    if (static_cast<int>(filtered.size()) > limit) {
        filtered.resize(limit);
    }
    return filtered;
}

bool PDTreeRouter::optimizeHBTInnerNodesForState(const Net& net,
                                                 PartialRouteState& state) const
{
    for (int i = 0; i < static_cast<int>(state.result.tree_nodes.size()); ++i) {
        TreeNodeState& node = state.result.tree_nodes[i];
        if (node.node_type != TreeNodeState::NodeType::kHBT) {
            continue;
        }
        const auto ranked = evaluateHBTNodeCandidates(net, state, i);
        if (ranked.empty()) {
            continue;
        }
        node.assigned_hbt_id = ranked.front();
        node.hbt_assigned = true;
        const auto& slot = grid_.hbt.getSlots().at(node.assigned_hbt_id);
        node.point.x = slot.x;
        node.point.y = slot.y;
        node.hbt_res = db_.getHBTResistanceOrDefault();
        node.hbt_cap = params_.hbt_cap;
        state.local_reserved_hbts.insert(node.assigned_hbt_id);
    }
    return materializeHBTNodesToSegments(net, state);
}

std::vector<int> PDTreeRouter::evaluateHBTNodeCandidates(const Net& /*net*/,
                                                          const PartialRouteState& state,
                                                          int hbt_tree_node) const
{
    if (hbt_tree_node < 0 || hbt_tree_node >= static_cast<int>(state.result.tree_nodes.size())) {
        return {};
    }
    const TreeNodeState& node = state.result.tree_nodes[hbt_tree_node];
    std::vector<int> candidates = node.candidate_hbt_ids;
    if (candidates.empty() && node.assigned_hbt_id >= 0) {
        candidates.push_back(node.assigned_hbt_id);
    }
    std::vector<std::pair<double, int>> scored;
    for (int hid : candidates) {
        scored.push_back({estimateHBTNodeCost(Net{}, state, hbt_tree_node, hid), hid});
    }
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<int> ranked;
    for (const auto& p : scored) {
        ranked.push_back(p.second);
    }
    return ranked;
}

double PDTreeRouter::estimateHBTNodeCost(const Net& /*net*/,
                                         const PartialRouteState& state,
                                         int hbt_tree_node,
                                         int candidate_hbt_id) const
{
    if (candidate_hbt_id < 0 || candidate_hbt_id >= static_cast<int>(grid_.hbt.getSlots().size())) {
        return std::numeric_limits<double>::infinity();
    }
    const auto& slots = grid_.hbt.getSlots();
    const auto& node = state.result.tree_nodes[hbt_tree_node];
    const auto& slot = slots[candidate_hbt_id];
    double wl = 0.0;
    if (node.parent_index >= 0 && node.parent_index < static_cast<int>(state.result.tree_nodes.size())) {
        wl += manhattan(state.result.tree_nodes[node.parent_index].point.x,
                        state.result.tree_nodes[node.parent_index].point.y,
                        slot.x, slot.y);
    }
    int fanout = 0;
    for (int i = 0; i < static_cast<int>(state.result.tree_nodes.size()); ++i) {
        if (state.result.tree_nodes[i].parent_index == hbt_tree_node) {
            wl += manhattan(state.result.tree_nodes[i].point.x,
                            state.result.tree_nodes[i].point.y,
                            slot.x, slot.y);
            ++fanout;
        }
    }
    const double depth_penalty = params_.weight_hbt_depth * static_cast<double>(node.depth);
    const double stack_penalty = params_.weight_hbt_stack * static_cast<double>(node.hbt_count_from_root);
    const double fanout_penalty = params_.weight_hbt_fanout * static_cast<double>(fanout);
    const double scarcity = params_.weight_hbt_scarcity * computeHBTScarcityPenalty(state.local_reserved_hbts, candidate_hbt_id);
    return params_.weight_hbt_subtree_wire * wl
         + params_.weight_hbt_subtree_delay_proxy * (db_.getHBTResistanceOrDefault() + 0.5 * params_.hbt_cap)
         + depth_penalty + stack_penalty + fanout_penalty + scarcity;
}

double PDTreeRouter::computeHBTScarcityPenalty(const std::unordered_set<int>& local_reserved_hbts,
                                               int hbt_id) const
{
    if (hbt_id < 0 || hbt_id >= static_cast<int>(grid_.hbt.getSlots().size())) {
        return 1.0;
    }
    const auto& slot = grid_.hbt.getSlots().at(hbt_id);
    auto nearby = grid_.hbt.querySlotsInBox(slot.x - std::max(1, db_.hbt.pitch_x),
                                            slot.y - std::max(1, db_.hbt.pitch_y),
                                            slot.x + std::max(1, db_.hbt.pitch_x),
                                            slot.y + std::max(1, db_.hbt.pitch_y));
    int avail = 0;
    for (int hid : nearby) {
        if (hid >= 0 && hid < static_cast<int>(hbt_used_.size()) && !hbt_used_[hid]
            && local_reserved_hbts.find(hid) == local_reserved_hbts.end()) {
            ++avail;
        }
    }
    return 1.0 / static_cast<double>(std::max(1, avail));
}

bool PDTreeRouter::materializeHBTNodesToSegments(const Net& /*net*/, PartialRouteState& state) const
{
    for (int i = 0; i < static_cast<int>(state.result.tree_nodes.size()); ++i) {
        TreeNodeState& node = state.result.tree_nodes[i];
        if (node.node_type != TreeNodeState::NodeType::kHBT || !node.hbt_assigned || node.assigned_hbt_id < 0) {
            continue;
        }
        const auto& s = grid_.hbt.getSlots().at(node.assigned_hbt_id);
        node.point.x = s.x;
        node.point.y = s.y;
        node.incoming_hbt_count = 1;
        node.incoming_hbt_res = node.hbt_res > 0.0 ? node.hbt_res : db_.getHBTResistanceOrDefault();
        node.incoming_hbt_cap = node.hbt_cap > 0.0 ? node.hbt_cap : params_.hbt_cap;
    }
    return true;
}

double PDTreeRouter::evaluateHBTPositionCost(const Net& /*net*/,
                                             const NetRouteResult& result) const
{
    double cost = 0.0;
    for (int i = 0; i < static_cast<int>(result.tree_nodes.size()); ++i) {
        const TreeNodeState& n = result.tree_nodes[i];
        if (n.node_type != TreeNodeState::NodeType::kHBT) {
            continue;
        }
        int fanout = 0;
        for (const auto& c : result.tree_nodes) {
            if (c.parent_index == i) {
                ++fanout;
            }
        }
        const double pos = static_cast<double>(std::abs(n.point.x - result.tree_nodes[result.root_tree_index].point.x)
                                             + std::abs(n.point.y - result.tree_nodes[result.root_tree_index].point.y));
        cost += params_.weight_hbt_depth * static_cast<double>(n.depth)
              + params_.weight_hbt_stack * static_cast<double>(n.hbt_count_from_root)
              + params_.weight_hbt_fanout * static_cast<double>(fanout)
              + params_.weight_hbt_scarcity * computeHBTScarcityPenalty({}, n.assigned_hbt_id)
              + params_.weight_hbt_position * pos;
    }
    return cost;
}

bool PDTreeRouter::build2DManhattanConnection(const RoutedPoint& a,
                                              const RoutedPoint& b,
                                              std::vector<RoutedSegment>& out_segments) const
{
    out_segments.clear();
    const DieId die_a = normalizeDie(a.die);
    const DieId die_b = normalizeDie(b.die);
    // Hard guard: ordinary 2D wires cannot be created with unknown die.
    if (die_a == DieId::kUnknown || die_b == DieId::kUnknown) {
        if (params_.verbose) {
            std::cerr << "[PDTreeRouter][ERROR] build2DManhattanConnection unknown die rejected\n";
        }
        return false;
    }

    // Non-HBT wire segments must stay on the same die.
    if (die_a != die_b) {
        if (params_.verbose) {
            std::cerr << "[PDTreeRouter][ERROR] build2DManhattanConnection cross-die rejected: "
                      << "a=(" << a.x << "," << a.y << "," << static_cast<int>(a.die) << "), "
                      << "b=(" << b.x << "," << b.y << "," << static_cast<int>(b.die) << ")"
                      << std::endl;
        }
        return false;
    }

    if (a.x == b.x || a.y == b.y) {
        RoutedSegment s;
        s.p1 = a;
        s.p2 = b;
        out_segments.push_back(s);
        return true;
    }

    RoutedPoint m1{b.x, a.y, a.die};
    RoutedPoint m2{a.x, b.y, a.die};

    const int len1 = manhattan(a, m1) + manhattan(m1, b);
    const int len2 = manhattan(a, m2) + manhattan(m2, b);
    const RoutedPoint mid = (len1 <= len2) ? m1 : m2;

    RoutedSegment s1;
    s1.p1 = a;
    s1.p2 = mid;

    RoutedSegment s2;
    s2.p1 = mid;
    s2.p2 = b;

    out_segments.push_back(s1);
    out_segments.push_back(s2);
    return true;
}

bool PDTreeRouter::build3DConnectionViaHBT(const RoutedPoint& parent_pt,
                                           const Pin& sink_pin,
                                           int hbt_id,
                                           std::vector<RoutedSegment>& out_segments) const
{
    out_segments.clear();

    const auto& slots = grid_.hbt.getSlots();
    if (hbt_id < 0 || hbt_id >= static_cast<int>(slots.size())) {
        return false;
    }

    const RoutedPoint sink_pt = pinToPoint(sink_pin);
    if (normalizeDie(parent_pt.die) == normalizeDie(sink_pt.die)) {
        return false;
    }

    const auto& h = slots[hbt_id];
    RoutedPoint h_parent{h.x, h.y, normalizeDie(parent_pt.die)};
    RoutedPoint h_sink{h.x, h.y, normalizeDie(sink_pt.die)};

    std::vector<RoutedSegment> seg1;
    std::vector<RoutedSegment> seg2;
    if (!build2DManhattanConnection(parent_pt, h_parent, seg1)) {
        return false;
    }
    if (!build2DManhattanConnection(h_sink, sink_pt, seg2)) {
        return false;
    }

    out_segments.insert(out_segments.end(), seg1.begin(), seg1.end());

    RoutedSegment vertical;
    vertical.p1 = h_parent;
    vertical.p2 = h_sink;
    vertical.uses_hbt = true;
    vertical.hbt_id = hbt_id;
    out_segments.push_back(vertical);

    out_segments.insert(out_segments.end(), seg2.begin(), seg2.end());
    return true;
}

bool PDTreeRouter::commitCrossDieBranch(NetRouteResult& result,
                                        int parent_tree_index,
                                        int sink_pin_index,
                                        int assigned_hbt_id) const
{
    const int seg_checkpoint = static_cast<int>(result.segments.size());
    const int node_checkpoint = static_cast<int>(result.tree_nodes.size());
    auto rollback = [&]() {
        result.segments.resize(seg_checkpoint);
        result.tree_nodes.resize(node_checkpoint);
    };

    if (parent_tree_index < 0 || parent_tree_index >= node_checkpoint) {
        return false;
    }
    const auto& slots = grid_.hbt.getSlots();
    if (assigned_hbt_id < 0 || assigned_hbt_id >= static_cast<int>(slots.size())) {
        return false;
    }
    const Net* net_ptr = nullptr;
    for (const auto& n : db_.nets) {
        if (n.name == result.net_name) {
            net_ptr = &n;
            break;
        }
    }
    if (net_ptr == nullptr || sink_pin_index < 0 || sink_pin_index >= static_cast<int>(net_ptr->pins.size())) {
        return false;
    }

    const RoutedPoint parent_pt = result.tree_nodes[parent_tree_index].point;
    const RoutedPoint sink_pt = pinToPoint(net_ptr->pins[sink_pin_index]);
    const DieId parent_die = normalizeDie(parent_pt.die);
    const DieId sink_die = normalizeDie(sink_pt.die);
    if (parent_die == sink_die) {
        return false;
    }
    const auto& slot = slots[assigned_hbt_id];
    const RoutedPoint hbt_from{slot.x, slot.y, parent_die};
    const RoutedPoint hbt_to{slot.x, slot.y, sink_die};

    std::vector<RoutedSegment> parent_to_hbt;
    std::vector<RoutedSegment> hbt_to_sink;
    if (!build2DManhattanConnection(parent_pt, hbt_from, parent_to_hbt)
        || !build2DManhattanConnection(hbt_to, sink_pt, hbt_to_sink)) {
        rollback();
        return false;
    }

    const int seg_begin_hbt = static_cast<int>(result.segments.size());
    result.segments.insert(result.segments.end(), parent_to_hbt.begin(), parent_to_hbt.end());
    RoutedSegment vertical;
    vertical.p1 = hbt_from;
    vertical.p2 = hbt_to;
    vertical.uses_hbt = true;
    vertical.hbt_id = assigned_hbt_id;
    result.segments.push_back(vertical);

    TreeNodeState hbt_node;
    hbt_node.node_type = TreeNodeState::NodeType::kHBT;
    hbt_node.point = hbt_to;
    hbt_node.assigned_hbt_id = assigned_hbt_id;
    hbt_node.hbt_assigned = true;
    hbt_node.parent_index = parent_tree_index;
    hbt_node.hbt_from_die = parent_die;
    hbt_node.hbt_to_die = sink_die;
    hbt_node.incoming_hbt_count = 1;
    hbt_node.incoming_hbt_res = params_.hbt_res;
    hbt_node.incoming_hbt_cap = params_.hbt_cap;
    hbt_node.incoming_segment_begin = seg_begin_hbt;
    hbt_node.incoming_segment_count = static_cast<int>(parent_to_hbt.size()) + 1;
    const TreeNodeState& p = result.tree_nodes[parent_tree_index];
    hbt_node.depth = p.depth + 1;
    hbt_node.path_length_from_root = p.path_length_from_root;
    hbt_node.hbt_count_from_root = p.hbt_count_from_root + 1;
    hbt_node.path_res_from_root = p.path_res_from_root + params_.hbt_res;
    hbt_node.path_cap_from_root = p.path_cap_from_root + params_.hbt_cap;
    hbt_node.path_delay_from_root = p.path_delay_from_root + 0.5 * params_.hbt_res * params_.hbt_cap;
    result.tree_nodes.push_back(hbt_node);
    const int hbt_node_index = static_cast<int>(result.tree_nodes.size()) - 1;

    commitSegments(result, hbt_to_sink, sink_pin_index, hbt_node_index, sink_pt, 0);
    if (result.tree_nodes.size() <= static_cast<size_t>(hbt_node_index + 1)) {
        rollback();
        return false;
    }
    return true;
}

bool PDTreeRouter::commit3DBranchWithHBTNode(NetRouteResult& result,
                                             int sink_pin_index,
                                             int parent_tree_index,
                                             int /*hbt_node_index*/,
                                             const std::vector<RoutedSegment>& seg_parent_to_hbt,
                                             const std::vector<RoutedSegment>& seg_hbt_to_sink,
                                             const RoutedPoint& /*hbt_point*/,
                                             const RoutedPoint& sink_point,
                                             const std::vector<int>& hbt_candidates,
                                             DieId from_die,
                                             DieId to_die,
                                             int assigned_hbt_id) const
{
    if (parent_tree_index < 0 || parent_tree_index >= static_cast<int>(result.tree_nodes.size())) {
        return false;
    }
    const auto& slots = grid_.hbt.getSlots();
    if (assigned_hbt_id < 0 || assigned_hbt_id >= static_cast<int>(slots.size())) {
        return false;
    }

    // HBT tree-node is topology/timing object. HBT segment is geometry object.
    // Both must use the same slot coordinates to keep tree/segments consistent.
    const auto& slot = slots.at(assigned_hbt_id);
    RoutedPoint hbt_from{slot.x, slot.y, normalizeDie(from_die)};
    RoutedPoint hbt_to{slot.x, slot.y, normalizeDie(to_die)};
    if (normalizeDie(hbt_from.die) == normalizeDie(hbt_to.die)) {
        return false;
    }

    std::vector<RoutedSegment> wire_to_hbt = seg_parent_to_hbt;
    std::vector<RoutedSegment> wire_from_hbt = seg_hbt_to_sink;
    if (wire_to_hbt.empty() || wire_to_hbt.back().p2.x != slot.x || wire_to_hbt.back().p2.y != slot.y) {
        wire_to_hbt.clear();
        if (!build2DManhattanConnection(result.tree_nodes[parent_tree_index].point, hbt_from, wire_to_hbt)) {
            return false;
        }
    }
    if (wire_from_hbt.empty() || wire_from_hbt.front().p1.x != slot.x || wire_from_hbt.front().p1.y != slot.y) {
        wire_from_hbt.clear();
        if (!build2DManhattanConnection(hbt_to, sink_point, wire_from_hbt)) {
            return false;
        }
    }

    const int seg_begin_hbt = static_cast<int>(result.segments.size());
    result.segments.insert(result.segments.end(), wire_to_hbt.begin(), wire_to_hbt.end());
    RoutedSegment vertical;
    vertical.p1 = hbt_from;
    vertical.p2 = hbt_to;
    vertical.uses_hbt = true;
    vertical.hbt_id = assigned_hbt_id;
    result.segments.push_back(vertical);

    TreeNodeState hbt_node;
    hbt_node.node_type = TreeNodeState::NodeType::kHBT;
    hbt_node.point = hbt_to;
    hbt_node.parent_index = parent_tree_index;
    hbt_node.assigned_hbt_id = assigned_hbt_id;
    hbt_node.hbt_assigned = true;
    hbt_node.candidate_hbt_ids = hbt_candidates;
    hbt_node.hbt_from_die = normalizeDie(from_die);
    hbt_node.hbt_to_die = normalizeDie(to_die);
    hbt_node.hbt_res = params_.hbt_res;
    hbt_node.hbt_cap = params_.hbt_cap;
    hbt_node.incoming_hbt_count = 1;
    hbt_node.incoming_hbt_res = params_.hbt_res;
    hbt_node.incoming_hbt_cap = params_.hbt_cap;
    hbt_node.incoming_segment_begin = seg_begin_hbt;
    hbt_node.incoming_segment_count = static_cast<int>(wire_to_hbt.size()) + 1;

    const TreeNodeState& p = result.tree_nodes[parent_tree_index];
    hbt_node.depth = p.depth + 1;
    hbt_node.path_length_from_root = p.path_length_from_root;
    hbt_node.hbt_count_from_root = p.hbt_count_from_root + 1;
    hbt_node.path_res_from_root = p.path_res_from_root + params_.hbt_res;
    hbt_node.path_cap_from_root = p.path_cap_from_root + params_.hbt_cap;
    hbt_node.path_delay_from_root = p.path_delay_from_root + 0.5 * params_.hbt_res * params_.hbt_cap;
    result.tree_nodes.push_back(hbt_node);
    const int hbt_tree_idx = static_cast<int>(result.tree_nodes.size()) - 1;

    // Topology must be parent -> HBT node -> sink node.
    commitSegments(result, wire_from_hbt, sink_pin_index, hbt_tree_idx, sink_point, 0);
    return true;
}

void PDTreeRouter::commitSegments(NetRouteResult& result,
                                  const std::vector<RoutedSegment>& segs,
                                  int sink_pin_index,
                                  int parent_tree_index,
                                  const RoutedPoint& sink_attach_point,
                                  int extra_hbt_count) const
{
    const double hbt_res_per_crossing = db_.getHBTResistanceOrDefault();
    const int seg_begin = static_cast<int>(result.segments.size());
    result.segments.insert(result.segments.end(), segs.begin(), segs.end());

    TreeNodeState n;
    n.pin_index = sink_pin_index;
    n.point = sink_attach_point;
    n.node_type = (sink_pin_index >= 0) ? TreeNodeState::NodeType::kPin : TreeNodeState::NodeType::kSteiner;
    n.parent_index = parent_tree_index;
    n.incoming_segment_begin = seg_begin;
    n.incoming_segment_count = static_cast<int>(segs.size());

    const auto& p = result.tree_nodes[parent_tree_index];
    n.depth = p.depth + 1;

    // IMPORTANT:
    //   Branch statistics must come from committed routed segments rather than
    //   parent-sink Manhattan shortcut. This keeps cross-die/HBT paths accurate.
    for (const RoutedSegment& s : segs) {
        if (s.uses_hbt) {
            ++n.incoming_hbt_count;
            // HBT resistance priority: DB hb_layer/via first, fallback defaults second.
            n.incoming_hbt_res += hbt_res_per_crossing;
            n.incoming_hbt_cap += params_.hbt_cap;
            continue;
        }

        const int seg_len_dbu = manhattan(s.p1, s.p2);
        n.incoming_wire_length += seg_len_dbu;

        // NOTE:
        //   This is still a global-routing approximation. We use directional
        //   layer-aware proxy unit RC (horizontal/vertical preferred). This is
        //   not detailed final layer assignment.
        const bool horizontal = std::abs(s.p1.x - s.p2.x) >= std::abs(s.p1.y - s.p2.y);
        const EffectiveRC rc = db_.computeDirectionalRCForDie(normalizeDie(s.p1.die), horizontal);
        const double seg_len_um = db_.dbuToMicronLength(seg_len_dbu);
        n.incoming_wire_res += rc.unit_res * seg_len_um;
        n.incoming_wire_cap += rc.unit_cap * seg_len_um;
    }

    if (n.incoming_hbt_count == 0 && extra_hbt_count > 0) {
        n.incoming_hbt_count = extra_hbt_count;
        n.incoming_hbt_res += hbt_res_per_crossing * static_cast<double>(extra_hbt_count);
        n.incoming_hbt_cap += params_.hbt_cap * static_cast<double>(extra_hbt_count);
    }

    n.path_length_from_root = p.path_length_from_root + n.incoming_wire_length;
    n.hbt_count_from_root = p.hbt_count_from_root + n.incoming_hbt_count;
    n.path_res_from_root = p.path_res_from_root + n.incoming_wire_res + n.incoming_hbt_res;
    n.path_cap_from_root = p.path_cap_from_root + n.incoming_wire_cap + n.incoming_hbt_cap;
    n.path_delay_from_root = p.path_delay_from_root + n.path_res_from_root * (n.incoming_wire_cap + n.incoming_hbt_cap);

    result.tree_nodes.push_back(n);
}


bool PDTreeRouter::isHBTAvailableForNet(int hbt_id,
                                        const std::string& net_name,
                                        const std::unordered_set<int>& local_reserved_hbts) const
{
    if (hbt_id < 0 || hbt_id >= static_cast<int>(hbt_used_.size())) {
        return false;
    }

    if (local_reserved_hbts.find(hbt_id) != local_reserved_hbts.end()) {
        return false;
    }

    if (!hbt_used_[hbt_id]) {
        return true;
    }

    return hbt_owner_[hbt_id] == net_name;
}

void PDTreeRouter::collectHBTsFromSegments(const std::vector<RoutedSegment>& segs,
                                           std::unordered_set<int>& out_hbts) const
{
    for (const RoutedSegment& s : segs) {
        if (s.uses_hbt && s.hbt_id >= 0) {
            out_hbts.insert(s.hbt_id);
        }
    }
}

void PDTreeRouter::commitNetLevelHBTReservation(const std::unordered_set<int>& local_reserved_hbts,
                                                const std::string& net_name)
{
    for (int hid : local_reserved_hbts) {
        if (hid < 0 || hid >= static_cast<int>(hbt_used_.size())) {
            continue;
        }
        hbt_used_[hid] = true;
        hbt_owner_[hid] = net_name;
    }
}

RoutedPoint PDTreeRouter::pinToPoint(const Pin& pin) const
{
    RoutedPoint p;
    p.x = clampInt(pin.x, db_.die_lx, db_.die_ux);
    p.y = clampInt(pin.y, db_.die_ly, db_.die_uy);
    p.die = pin.die;
    if (p.die == DieId::kUnknown && params_.verbose) {
        std::cerr << "[PDTreeRouter][ERROR] pinToPoint sees unknown die pin="
                  << pin.name << " node=" << pin.node_name << std::endl;
    }
    return p;
}

DieId PDTreeRouter::determine2DNetDie(const Net& net, int source_pin_index) const
{
    if (source_pin_index >= 0 && source_pin_index < static_cast<int>(net.pins.size())) {
        const DieId src_die = net.pins[source_pin_index].die;
        if (src_die == DieId::kTop || src_die == DieId::kBottom) {
            return src_die;
        }
    }

    for (const Pin& pin : net.pins) {
        if (pin.die == DieId::kTop || pin.die == DieId::kBottom) {
            return pin.die;
        }
    }

    return DieId::kTop;
}

DieId PDTreeRouter::resolve2DPinDie(const Pin& pin, DieId fallback_die) const
{
    if (pin.die == DieId::kTop || pin.die == DieId::kBottom) {
        return pin.die;
    }
    return fallback_die;
}

DieId PDTreeRouter::resolve2DPointDie(const RoutedPoint& pt, DieId fallback_die) const
{
    if (pt.die == DieId::kTop || pt.die == DieId::kBottom) {
        return pt.die;
    }
    return fallback_die;
}

int PDTreeRouter::manhattan(int x1, int y1, int x2, int y2) const
{
    return std::abs(x1 - x2) + std::abs(y1 - y2);
}

int PDTreeRouter::manhattan(const RoutedPoint& a, const RoutedPoint& b) const
{
    // Keep all db_/member accesses inside member-function scope only.
    return manhattan(a.x, a.y, b.x, b.y);
}

RouteValidationResult PDTreeRouter::validateRouteResultTopology(const NetRouteResult& result) const
{
    RouteValidationResult vr;
    const auto& slots = grid_.hbt.getSlots();
    std::unordered_map<long long, std::vector<int>> adj;
    auto key=[](const RoutedPoint& p){ return (static_cast<long long>(p.x)<<32) ^ (static_cast<long long>(p.y)<<8) ^ static_cast<int>(normalizeDie(p.die)); };

    for (const auto& seg : result.segments) {
        if (normalizeDie(seg.p1.die) == DieId::kUnknown || normalizeDie(seg.p2.die) == DieId::kUnknown) {
            vr.errors.push_back("unknown_die_in_segment");
            vr.valid = false;
        }
        if (!seg.uses_hbt && normalizeDie(seg.p1.die) != normalizeDie(seg.p2.die)) {
            vr.non_hbt_cross_die_segments++;
            vr.errors.push_back("non_hbt_cross_die_segment");
        }
        if (seg.uses_hbt) {
            if (seg.p1.x != seg.p2.x || seg.p1.y != seg.p2.y || normalizeDie(seg.p1.die)==normalizeDie(seg.p2.die) || seg.hbt_id < 0 || seg.hbt_id >= static_cast<int>(slots.size())) {
                vr.invalid_hbt_segments++;
                vr.errors.push_back("invalid_hbt_segment");
            }
        }
        adj[key(seg.p1)].push_back(static_cast<int>(adj[key(seg.p1)].size()));
        adj[key(seg.p2)].push_back(static_cast<int>(adj[key(seg.p2)].size()));
    }
    vr.disconnected_components = result.segments.empty() ? 0 : 1;
    for (const auto& n : result.tree_nodes) {
        if (n.node_type != TreeNodeState::NodeType::kHBT) continue;
        if (n.assigned_hbt_id < 0 || n.assigned_hbt_id >= static_cast<int>(slots.size())) { vr.hbt_node_segment_mismatches++; continue; }
        const auto& slot = slots[n.assigned_hbt_id];
        bool matched = false;
        for (const auto& seg : result.segments) {
            if (seg.uses_hbt && seg.hbt_id == n.assigned_hbt_id && seg.p1.x == slot.x && seg.p1.y == slot.y) { matched = true; break; }
        }
        if (!matched || n.point.x != slot.x || n.point.y != slot.y || !n.hbt_assigned) {
            vr.hbt_node_segment_mismatches++;
            vr.errors.push_back("hbt_node_segment_mismatch");
        }
    }
    for (size_t i = 0; i < result.tree_nodes.size(); ++i) {
        const int pidx = result.tree_nodes[i].parent_index;
        if (i == static_cast<size_t>(result.root_tree_index)) {
            continue;
        }
        if (pidx < 0 || pidx >= static_cast<int>(result.tree_nodes.size())) {
            vr.errors.push_back("invalid_parent_index");
            vr.valid = false;
        }
    }
    if (vr.non_hbt_cross_die_segments || vr.invalid_hbt_segments || vr.hbt_node_segment_mismatches) {
        vr.valid = false;
    }
    return vr;
}
