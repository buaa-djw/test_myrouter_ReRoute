#include "NetReRoute.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <queue>
#include <unordered_map>

namespace
{
    DieId normalizeDieLocal(DieId d) { return (d == DieId::kTop || d == DieId::kBottom) ? d : DieId::kTop; }
}
CriticalNetOptimizer::CriticalNetOptimizer(const RouterDB &db, const HybridGrid &grid, const PDTreeRouter &router, HBTResourceManager &hbt_manager, const Params &params) : db_(db), grid_(grid), router_(router), hbt_manager_(hbt_manager), params_(params) {}

std::vector<CriticalNetOptimizer::CriticalNetRecord> CriticalNetOptimizer::collectCriticalNets(const std::vector<NetRouteResult> &results) const
{
    std::unordered_map<std::string, int> m;
    for (int i = 0; i < (int)db_.nets.size(); ++i)
        m[db_.nets[i].name] = i;
    std::vector<CriticalNetRecord> r;
    for (int i = 0; i < (int)results.size(); ++i)
    {
        auto &rr = results[i];
        if (!rr.success || !rr.delay_summary.ready)
            continue;
        auto it = m.find(rr.net_name);
        const Net* net_ptr = (it == m.end()) ? nullptr : &db_.nets[it->second];
        const bool routed_as_3d = resultIsRoutedAs3D(rr, net_ptr);
        if (params_.target_net_type == "3d_only" && !routed_as_3d)
            continue;
        if (params_.target_net_type == "2d_only" && routed_as_3d)
            continue;
        if (it == m.end())
            continue;
        r.push_back({i, it->second, rr.net_name, rr.delay_summary.avg_sink_delay, rr.delay_summary.max_sink_delay, rr.delay_summary.max_delay_pin_index});
    }
    std::sort(r.begin(), r.end(), [](auto &a, auto &b)
              { return a.max_delay > b.max_delay; });
    if ((int)r.size() > params_.top_k_nets)
        r.resize(params_.top_k_nets);
    return r;
}

CriticalNetOptimizer::OptimizationStats CriticalNetOptimizer::optimize(std::vector<NetRouteResult> &results) const
{
    OptimizationStats s;
    hbt_manager_.validateNoConflict(&std::cout);
    if (params_.verbose) std::cout << "[NetReRoute] start target_net_type=" << params_.target_net_type << " enable_hbt_swap=" << params_.enable_hbt_swap << " force_accept_hbt_swap=" << params_.debug_force_accept_hbt_swap << "\n";
    for (auto &rec : collectCriticalNets(results))
    {
        auto &rr = results[rec.result_index];
        const auto &net = db_.nets[rec.net_index];
        s.visited_nets++;
        const bool routed_as_3d = resultIsRoutedAs3D(rr, &net);
        if (routed_as_3d) s.visited_3d_nets++; else s.visited_2d_nets++;
        s.total_objective_before += evaluatePostOptimizationObjective(rr, net);
        optimizeOneNet(net, rr, s);
        s.total_objective_after += evaluatePostOptimizationObjective(rr, net);
    }
    if (params_.verbose) std::cout << "[NetReRoute][summary] visited_3d=" << s.visited_3d_nets << " tried_hbt_swap=" << s.tried_hbt_swap_candidates << " accepted_hbt_swap=" << s.accepted_hbt_swap_candidates << " changed_hbt_id=" << s.changed_hbt_id_count << "\n";
    return s;
}

bool CriticalNetOptimizer::optimizeOneNet(const Net &net, NetRouteResult &result, OptimizationStats &stats) const
{
    if (!result.success || !result.delay_summary.ready || result.delay_summary.single_pin_net)
        return false;
    int sink_pin = result.delay_summary.max_delay_pin_index;
    int sink_tree = findTreeNodeForPin(result, sink_pin);
    if (sink_tree < 0 || sink_tree == result.root_tree_index)
        return false;
    double base_obj = evaluatePostOptimizationObjective(result, net);
    const double base_max_delay = result.delay_summary.max_sink_delay;
    auto base_t = router_.evaluateTimingSummaryPublic(net, result);
    std::vector<NetEditCandidate> cands;
    if (params_.enable_reattach)
    {
        for (int p : router_.collectCandidateParentsPublic(net, result, net.pins[sink_pin]))
        {
            if ((int)cands.size() >= params_.max_iterations_per_net)
                break;
            if (p == sink_tree || wouldCreateCycle(result, p, sink_tree))
            {
                stats.rejected_by_cycle++;
                continue;
            }
            std::vector<RoutedSegment> segs;
            auto pp = result.tree_nodes[p].point;
            auto sp = router_.pinToPointPublic(net.pins[sink_pin]);
            if (normalizeDieLocal(pp.die) != normalizeDieLocal(sp.die))
            {
                stats.rejected_by_cross_die_not_supported++;
                continue;
            }
            if (!router_.build2DConnectionPublic(pp, sp, segs))
                continue;
            NetEditCandidate c;
            c.type = EditType::kReattachSinkSameDie;
            c.sink_pin_index = sink_pin;
            c.sink_tree_node = sink_tree;
            c.old_parent_tree_node = result.tree_nodes[sink_tree].parent_index;
            c.new_parent_tree_node = p;
            c.new_segments = segs;
            cands.push_back(c);
        }
    }
    if (params_.enable_ripup)
    {
        auto rs = generateRipupOneSinkCandidates(net, result, sink_pin, sink_tree, &stats);
        cands.insert(cands.end(), rs.begin(), rs.end());
    }
    if (params_.enable_hbt_swap)
    {
        auto hs = generateHBTSwapCandidates(net, result, sink_pin, sink_tree, &stats);
        cands.insert(cands.end(), hs.begin(), hs.end());
    }
    if (cands.empty())
        return false;
    NetRouteResult best = result;
    bool found = false;
    NetEditCandidate bestc;
    double best_obj = base_obj;
    for (auto &c : cands)
    {
        stats.tried_candidates++;
        if (c.type == EditType::kRipupOneSinkBranch) stats.tried_ripup_candidates++;
        else if (c.type == EditType::kReattachSinkSameDie) stats.tried_reattach_candidates++;

        else if (c.type == EditType::kCrossDieRipupViaHBT) stats.tried_cross_die_ripup_candidates++;
        else if (c.type == EditType::kInsertHBT) stats.tried_hbt_insert_candidates++;
        else if (c.type == EditType::kRemoveHBT) stats.tried_hbt_remove_candidates++;
        auto snap = result;
        auto hs = hbt_manager_.makeSnapshot();
        std::string fr;
        if (!applyRipupCandidate(net, result, c, hbt_manager_, fr))
        {
            c.reject_reason = fr;
            rollbackToSnapshot(result, snap, hbt_manager_, hs);
            continue;
        }
        auto t = router_.evaluateTimingSummaryPublic(net, result);
        double obj = evaluatePostOptimizationObjective(result, net);
        if (params_.verbose && c.type == EditType::kSwapHBT) {
            std::cout << "[NetReRoute][HBT-SWAP][candidate] net=" << net.name << " old_hbt=" << c.old_hbt_id
                      << " new_hbt=" << c.new_hbt_id << " old_wl=" << base_t.total_wirelength
                      << " new_wl=" << t.total_wirelength << " old_delay=" << snap.delay_summary.max_sink_delay
                      << " new_delay=" << result.delay_summary.max_sink_delay << " old_obj=" << base_obj
                      << " new_obj=" << obj << "\n";
        }
        bool wl_ok = t.total_wirelength <= base_t.total_wirelength * (1.0 + params_.max_wirelength_growth_ratio) + 1e-9;
        bool hbt_ok = t.hbt_count <= base_t.hbt_count + params_.max_extra_hbts;
        bool delay_ok = result.delay_summary.ready && result.delay_summary.max_sink_delay <= snap.delay_summary.max_sink_delay + 1e-12;
        bool force_mode = (c.type == EditType::kSwapHBT && params_.debug_force_accept_hbt_swap);
        if (!wl_ok && !force_mode)
        {
            stats.rejected_by_wirelength++;
            if (c.type == EditType::kSwapHBT) {
                std::cout << "[NetReRoute][reject] net=" << net.name << " type=hbt_swap old_hbt=" << c.old_hbt_id
                          << " new_hbt=" << c.new_hbt_id << " reason=wirelength obj_before=" << base_obj
                          << " obj_after=" << obj << " wl_before=" << base_t.total_wirelength
                          << " wl_after=" << t.total_wirelength << " delay_before=" << snap.delay_summary.max_sink_delay
                          << " delay_after=" << result.delay_summary.max_sink_delay << "\n";
            }
            rollbackToSnapshot(result, snap, hbt_manager_, hs);
            continue;
        }
        if (!hbt_ok && !force_mode)
        {
            stats.rejected_by_hbt_conflict++;
            if (c.type == EditType::kSwapHBT) {
                std::cout << "[NetReRoute][reject] net=" << net.name << " type=hbt_swap old_hbt=" << c.old_hbt_id
                          << " new_hbt=" << c.new_hbt_id << " reason=hbt_conflict obj_before=" << base_obj
                          << " obj_after=" << obj << " wl_before=" << base_t.total_wirelength
                          << " wl_after=" << t.total_wirelength << " delay_before=" << snap.delay_summary.max_sink_delay
                          << " delay_after=" << result.delay_summary.max_sink_delay << "\n";
            }
            rollbackToSnapshot(result, snap, hbt_manager_, hs);
            continue;
        }
        if (!delay_ok && !force_mode)
        {
            stats.rejected_by_delay++;
            if (c.type == EditType::kSwapHBT) {
                std::cout << "[NetReRoute][reject] net=" << net.name << " type=hbt_swap old_hbt=" << c.old_hbt_id
                          << " new_hbt=" << c.new_hbt_id << " reason=delay obj_before=" << base_obj
                          << " obj_after=" << obj << " wl_before=" << base_t.total_wirelength
                          << " wl_after=" << t.total_wirelength << " delay_before=" << snap.delay_summary.max_sink_delay
                          << " delay_after=" << result.delay_summary.max_sink_delay << "\n";
            }
            rollbackToSnapshot(result, snap, hbt_manager_, hs);
            continue;
        }
        bool force_ok = force_mode;
        if (force_ok) {
            if (!result.delay_summary.ready) force_ok = false;
            if (force_ok && !result.validation.valid) force_ok = false;
            if (force_ok && !hbt_ok) force_ok = false;
            std::string vr; if (force_ok) force_ok = verifyHBTSwapApplied(result, c.old_hbt_id, c.new_hbt_id, vr);
            if (!force_ok) {
                stats.rejected_by_hbt_swap_not_applied++;
                std::cout << "[NetReRoute][reject] net=" << net.name << " type=hbt_swap old_hbt=" << c.old_hbt_id
                          << " new_hbt=" << c.new_hbt_id << " reason=not_applied obj_before=" << base_obj
                          << " obj_after=" << obj << " wl_before=" << base_t.total_wirelength
                          << " wl_after=" << t.total_wirelength << " delay_before=" << snap.delay_summary.max_sink_delay
                          << " delay_after=" << result.delay_summary.max_sink_delay << "\n";
            }
        }
        if (obj + 1e-12 < best_obj || force_ok)
        {
            found = true;
            best = result;
            best_obj = obj;
            bestc = c;
            if (force_ok) {
                stats.hbt_swap_force_accept_used = 1;
            }
        }
        rollbackToSnapshot(result, snap, hbt_manager_, hs);
    }
    if (!found)
        return false;
    std::string fr;
    if (!applyRipupCandidate(net, result, bestc, hbt_manager_, fr))
        return false;
    stats.accepted_candidates++;
    if (bestc.type == EditType::kRipupOneSinkBranch) stats.accepted_ripup_candidates++;
    else if (bestc.type == EditType::kReattachSinkSameDie) stats.accepted_reattach_candidates++;
    else if (bestc.type == EditType::kSwapHBT) stats.accepted_hbt_swap_candidates++;
    else if (bestc.type == EditType::kCrossDieRipupViaHBT) stats.accepted_cross_die_ripup_candidates++;
    else if (bestc.type == EditType::kInsertHBT) stats.accepted_hbt_insert_candidates++;
    else if (bestc.type == EditType::kRemoveHBT) stats.accepted_hbt_remove_candidates++;
    stats.improved_nets++;

    auto final_timing = router_.evaluateTimingSummaryPublic(net, result);
    result.reroute_info.touched = true;
    result.reroute_info.improved = (best_obj + 1e-12 < base_obj);
    result.reroute_info.edit_type = (bestc.type == EditType::kRipupOneSinkBranch) ? "ripup_one_sink" : (bestc.type == EditType::kSwapHBT ? "hbt_swap" : "reattach_same_die");
    result.reroute_info.delay_before = base_max_delay;
    result.reroute_info.delay_after = result.delay_summary.max_sink_delay;
    result.reroute_info.objective_before = base_obj;
    result.reroute_info.objective_after = best_obj;
    result.reroute_info.wirelength_before = base_t.total_wirelength;
    result.reroute_info.wirelength_after = final_timing.total_wirelength;
    result.reroute_info.hbt_count_before = base_t.hbt_count;
    result.reroute_info.hbt_count_after = final_timing.hbt_count;
    result.reroute_info.old_hbt_id = bestc.old_hbt_id;
    result.reroute_info.new_hbt_id = bestc.new_hbt_id;
    result.reroute_info.changed_hbt_id_count = bestc.changed_hbt_id_count;
    result.reroute_info.changed_segment_count = bestc.changed_segment_count;
    result.reroute_info.hbt_delay_before = base_t.hbt_delay;
    result.reroute_info.hbt_delay_after = final_timing.hbt_delay;
    result.reroute_info.force_accepted = (bestc.type == EditType::kSwapHBT && params_.debug_force_accept_hbt_swap);
    result.reroute_info.reject_reason.clear();
    if (bestc.type == EditType::kSwapHBT) {
        const int force = (bestc.type == EditType::kSwapHBT && params_.debug_force_accept_hbt_swap) ? 1 : 0;
        std::cout << "[NetReRoute][accept] net=" << net.name << " type=hbt_swap old_hbt=" << bestc.old_hbt_id
                  << " new_hbt=" << bestc.new_hbt_id << " force=" << force << " obj_before=" << base_obj
                  << " obj_after=" << best_obj << " delay_before=" << base_max_delay
                  << " delay_after=" << result.delay_summary.max_sink_delay << "\n";
    }
    return true;
}

int CriticalNetOptimizer::findTreeNodeForPin(const NetRouteResult &result, int pin) const
{
    for (int i = 0; i < (int)result.tree_nodes.size(); ++i)
        if (result.tree_nodes[i].pin_index == pin)
            return i;
    return -1;
}
bool CriticalNetOptimizer::isDescendantTreeNode(const NetRouteResult &result, int a, int n) const
{
    int cur = n;
    while (cur >= 0 && cur < (int)result.tree_nodes.size())
    {
        if (cur == a)
            return true;
        cur = result.tree_nodes[cur].parent_index;
    }
    return false;
}
bool CriticalNetOptimizer::wouldCreateCycle(const NetRouteResult &result, int np, int c) const { return np == c || isDescendantTreeNode(result, c, np); }

std::vector<CriticalNetOptimizer::NetEditCandidate> CriticalNetOptimizer::generateRipupOneSinkCandidates(const Net &net, const NetRouteResult &result, int sink_pin_index, int sink_tree_node, OptimizationStats *stats) const
{
    std::vector<NetEditCandidate> out;
    int oldp = result.tree_nodes[sink_tree_node].parent_index;
    const auto &sn = result.tree_nodes[sink_tree_node];
    std::vector<RoutedSegment> olds;
    for (int i = 0; i < sn.incoming_segment_count; ++i)
        olds.push_back(result.segments[sn.incoming_segment_begin + i]);
    auto parents = router_.collectCandidateParentsPublic(net, result, net.pins[sink_pin_index]);
    for (int p : parents)
    {
        if (p == sink_tree_node || p == oldp || wouldCreateCycle(result, p, sink_tree_node))
        {
            if (stats)
                stats->rejected_by_cycle++;
            continue;
        }
        std::vector<RoutedSegment> segs;
        auto pp = result.tree_nodes[p].point;
        auto sp = router_.pinToPointPublic(net.pins[sink_pin_index]);
        if (normalizeDieLocal(pp.die) != normalizeDieLocal(sp.die))
        {
            if (stats)
                stats->rejected_by_cross_die_not_supported++;
            continue;
        }
        if (!router_.build2DConnectionPublic(pp, sp, segs))
            continue;
        NetEditCandidate c;
        c.type = EditType::kRipupOneSinkBranch;
        c.sink_pin_index = sink_pin_index;
        c.sink_tree_node = sink_tree_node;
        c.old_parent_tree_node = oldp;
        c.new_parent_tree_node = p;
        c.old_segments = olds;
        c.new_segments = segs;
        for (auto &s : olds)
            if (s.uses_hbt)
                c.old_hbt_ids.push_back(s.hbt_id);
        for (auto &s : segs)
            if (s.uses_hbt)
                c.new_hbt_ids.push_back(s.hbt_id);
        out.push_back(c);
        if ((int)out.size() >= params_.max_iterations_per_net)
            break;
    }
    return out;
}

bool CriticalNetOptimizer::rollbackToSnapshot(NetRouteResult &result, const NetRouteResult &snap, HBTResourceManager &hm, const HBTResourceManager::Snapshot &hs) const
{
    result = snap;
    hm.rollback(hs);
    return true;
}
bool CriticalNetOptimizer::applyRipupCandidate(const Net &net, NetRouteResult &result, const NetEditCandidate &c, HBTResourceManager &hm, std::string &fr) const
{
    for (int id : c.old_hbt_ids)
        hm.release(id, net.name);
    for (int id : c.new_hbt_ids)
    {
        if (id < 0)
        {
            fr = "invalid_hbt";
            return false;
        }
        if (!hm.reserve(id, net.name, -1, c.sink_tree_node, c.sink_pin_index))
        {
            fr = "hbt_reserve_conflict";
            return false;
        }
    }
    RoutedPoint sink_attach = c.has_child_attach_point ? c.child_attach_point : router_.pinToPointPublic(net.pins[c.sink_pin_index]);
    if (!replaceSinkIncomingBranch(net, result, c.sink_tree_node, c.new_parent_tree_node, c.new_segments, sink_attach))
    {
        fr = "replace_branch_failed";
        return false;
    }
    if (!router_.annotateDelayPublic(net, result) || !result.delay_summary.ready)
    {
        fr = "edcompute_failed";
        return false;
    }
    result.validation = router_.validateRouteResultTopologyPublic(result);
    if (!result.validation.valid)
    {
        fr = "invalid_topology";
        return false;
    }
    return true;
}

bool CriticalNetOptimizer::replaceSinkIncomingBranch(const Net &, NetRouteResult &result, int sink_tree_node, int new_parent_tree_index, const std::vector<RoutedSegment> &new_segments, const RoutedPoint &sink_point) const
{
    auto &n = result.tree_nodes[sink_tree_node];
    int ob = n.incoming_segment_begin, oc = n.incoming_segment_count, oe = ob + oc;
    std::vector<RoutedSegment> reb;
    reb.insert(reb.end(), result.segments.begin(), result.segments.begin() + ob);
    reb.insert(reb.end(), new_segments.begin(), new_segments.end());
    reb.insert(reb.end(), result.segments.begin() + oe, result.segments.end());
    int d = (int)new_segments.size() - oc;
    for (auto &tn : result.tree_nodes)
        if (tn.incoming_segment_begin >= oe)
            tn.incoming_segment_begin += d;
    n.parent_index = new_parent_tree_index;
    n.incoming_segment_begin = ob;
    n.incoming_segment_count = (int)new_segments.size();
    n.point = sink_point;
    n.incoming_wire_length = 0;
    n.incoming_hbt_count = 0;
    n.incoming_wire_res = n.incoming_wire_cap = n.incoming_hbt_res = n.incoming_hbt_cap = 0;
    n.assigned_hbt_id = -1;
    for (auto &s : new_segments)
    {
        if (s.uses_hbt)
        {
            if (s.hbt_id < 0)
                return false;
            n.incoming_hbt_count++;
            n.assigned_hbt_id = s.hbt_id;
            n.incoming_hbt_res += db_.hbt.parasitic_res;
            n.incoming_hbt_cap += db_.hbt.parasitic_cap;
        }
        else
        {
            int len = std::abs(s.p1.x - s.p2.x) + std::abs(s.p1.y - s.p2.y);
            n.incoming_wire_length += len;
            auto rc = db_.computeEffectiveRCForDie(normalizeDieLocal(s.p1.die));
            double um = db_.dbuToMicronLength(len);
            n.incoming_wire_res += rc.unit_res * um;
            n.incoming_wire_cap += rc.unit_cap * um;
        }
    }
    result.segments = std::move(reb);
    return router_.validateRouteResultTopologyPublic(result).valid;
}

bool CriticalNetOptimizer::rebuildTreeStatistics(const Net &, NetRouteResult &result) const { return router_.validateRouteResultTopologyPublic(result).valid; }

double CriticalNetOptimizer::evaluatePostOptimizationObjective(const NetRouteResult &r, const Net &) const { return params_.objective_weight_max_delay * r.delay_summary.max_sink_delay + params_.objective_weight_avg_delay * r.delay_summary.avg_sink_delay + params_.objective_weight_hbt_delay * r.delay_summary.ed_hbt_delay_contrib + params_.objective_weight_hbt_count * r.delay_summary.total_hbt_cap + params_.objective_weight_wirelength_growth * r.delay_summary.total_wire_cap; }


std::vector<int> CriticalNetOptimizer::collectHBTsOnPath(const NetRouteResult& result, int sink_tree_node) const
{
    std::vector<int> ids;
    int cur = sink_tree_node;
    while (cur >= 0 && cur < (int)result.tree_nodes.size()) {
        const auto& tn = result.tree_nodes[cur];
        if (tn.assigned_hbt_id >= 0) ids.push_back(tn.assigned_hbt_id);
        for (int i = 0; i < tn.incoming_segment_count; ++i) {
            const auto& s = result.segments[tn.incoming_segment_begin + i];
            if (s.uses_hbt && s.hbt_id >= 0) ids.push_back(s.hbt_id);
        }
        cur = tn.parent_index;
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::vector<int> CriticalNetOptimizer::collectAllUsedHBTsInNet(const NetRouteResult& result) const {
    std::vector<int> ids;
    for (const auto& n : result.tree_nodes) {
        if (n.assigned_hbt_id >= 0) ids.push_back(n.assigned_hbt_id);
        if (n.incoming_hbt_count > 0 && n.assigned_hbt_id >= 0) ids.push_back(n.assigned_hbt_id);
    }
    for (const auto& s : result.segments) if (s.uses_hbt && s.hbt_id >= 0) ids.push_back(s.hbt_id);
    std::sort(ids.begin(), ids.end()); ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::vector<int> CriticalNetOptimizer::collectCandidateHBTsForReroute(const Net& net, const NetRouteResult&, const RoutedPoint& parent_point, const RoutedPoint& sink_point, int old_hbt_id, int max_count) const {
    std::vector<int> out;
    Point2D mid{(parent_point.x + sink_point.x)/2, (parent_point.y + sink_point.y)/2};
    auto add=[&](const RoutedPoint& rp){ auto v=hbt_manager_.collectFreeHBTsNear(grid_, Point2D{rp.x,rp.y}, max_count); out.insert(out.end(), v.begin(), v.end()); };
    if (old_hbt_id >= 0 && hbt_manager_.isOwnedBy(old_hbt_id, net.name)) out.push_back(old_hbt_id);
    auto m = hbt_manager_.collectFreeHBTsNear(grid_, mid, max_count); out.insert(out.end(), m.begin(), m.end());
    add(sink_point); add(parent_point);
    if ((int)out.size() < max_count) { auto all=hbt_manager_.collectAllFreeHBTs(); out.insert(out.end(), all.begin(), all.end()); }
    std::sort(out.begin(), out.end()); out.erase(std::unique(out.begin(), out.end()), out.end());
    if ((int)out.size() > max_count) out.resize(max_count);
    if (params_.verbose) std::cout << "[NetReRoute][HBT] collect candidates net=" << net.name << " old=" << old_hbt_id << " free=" << hbt_manager_.collectAllFreeHBTs().size() << " returned=" << out.size() << "\n";
    return out;
}

bool CriticalNetOptimizer::buildCrossDieBranchViaHBT(const RoutedPoint& parent_point, const RoutedPoint& sink_point, int hbt_id, std::vector<RoutedSegment>& out_segments, std::string& fail_reason) const {
    if (normalizeDieLocal(parent_point.die) == normalizeDieLocal(sink_point.die)) { fail_reason = "same_die"; return false; }
    Point2D hp; if (!grid_.getHBTPoint(hbt_id, hp)) { fail_reason = "invalid_hbt"; return false; }
    out_segments.clear(); RoutedPoint ph{hp.x,hp.y,parent_point.die}; RoutedPoint sh{hp.x,hp.y,sink_point.die}; std::vector<RoutedSegment> a,b;
    if (!router_.build2DConnectionPublic(parent_point, ph, a) || !router_.build2DConnectionPublic(sh, sink_point, b)) { fail_reason = "build2d_failed"; return false; }
    out_segments.insert(out_segments.end(), a.begin(), a.end());
    out_segments.push_back(RoutedSegment{ph, sh, -1, true, hbt_id});
    out_segments.insert(out_segments.end(), b.begin(), b.end());
    return true;
}

bool CriticalNetOptimizer::verifyHBTSwapApplied(const NetRouteResult& result, int old_hbt_id, int new_hbt_id, std::string& reason) const {
    bool has_new = false, has_new_node=false;
    for (const auto& s : result.segments) if (s.uses_hbt && s.hbt_id == new_hbt_id) has_new = true;
    for (const auto& n : result.tree_nodes) if (n.assigned_hbt_id == new_hbt_id) has_new_node=true;
    if (!has_new || !has_new_node) { reason = "hbt_swap_not_reflected_in_topology"; return false; }
    return true;
}

bool CriticalNetOptimizer::resultUsesHBT(const NetRouteResult& result) const {
    for (const auto& seg : result.segments) {
        if (seg.uses_hbt && seg.hbt_id >= 0) return true;
    }
    for (const auto& node : result.tree_nodes) {
        if (node.assigned_hbt_id >= 0 || node.incoming_hbt_count > 0) return true;
    }
    return false;
}

bool CriticalNetOptimizer::resultIsRoutedAs3D(const NetRouteResult& result, const Net* net) const {
    return result.is_3d || (net != nullptr && net->is_3d) || resultUsesHBT(result);
}

bool CriticalNetOptimizer::findBranchByHBTId(const NetRouteResult& result, int old_hbt_id, HBTBranchRef& out_ref) const {
    for (int child = 0; child < (int)result.tree_nodes.size(); ++child) {
        const auto& tn = result.tree_nodes[child];
        for (int i = 0; i < tn.incoming_segment_count; ++i) {
            const auto& s = result.segments[tn.incoming_segment_begin + i];
            if (s.uses_hbt && s.hbt_id == old_hbt_id) {
                int parent = tn.parent_index;
                if (parent < 0 || parent >= (int)result.tree_nodes.size()) return false;
                out_ref.old_hbt_id = old_hbt_id;
                out_ref.child_tree_node = child;
                out_ref.parent_tree_node = parent;
                out_ref.parent_point = result.tree_nodes[parent].point;
                out_ref.child_point = tn.point;
                out_ref.old_segments.clear();
                for (int j = 0; j < tn.incoming_segment_count; ++j) out_ref.old_segments.push_back(result.segments[tn.incoming_segment_begin + j]);
                return true;
            }
        }
    }
    return false;
}

std::vector<CriticalNetOptimizer::NetEditCandidate> CriticalNetOptimizer::generateHBTSwapCandidates(const Net &net, const NetRouteResult &result, int sink_pin_index, int sink_tree_node, OptimizationStats *stats) const {
    std::vector<NetEditCandidate> out;
    if (!resultIsRoutedAs3D(result, &net)) { if (stats) stats->rejected_by_non_3d_net++; if(params_.verbose) std::cout<<"[NetReRoute][HBT-SWAP][skip] net="<<net.name<<" reason=non_3d\n"; return out; }
    auto path_hbts = collectHBTsOnPath(result, sink_tree_node); auto all_hbts = collectAllUsedHBTsInNet(result);
    std::vector<int> old_hbts=path_hbts; old_hbts.insert(old_hbts.end(), all_hbts.begin(), all_hbts.end()); std::sort(old_hbts.begin(), old_hbts.end()); old_hbts.erase(std::unique(old_hbts.begin(), old_hbts.end()), old_hbts.end());
    if(params_.verbose) std::cout<<"[NetReRoute][HBT-SWAP][scan] net="<<net.name<<" path_hbts="<<path_hbts.size()<<" all_hbts="<<all_hbts.size()<<"\n";
    if (old_hbts.empty()) { if (stats) stats->rejected_by_no_hbt_on_path++; return out; }
    for (int old_id : old_hbts) {
        HBTBranchRef branch;
        if (!findBranchByHBTId(result, old_id, branch)) continue;
        if (params_.verbose) std::cout << "[NetReRoute][HBT-SWAP][branch] net=" << net.name << " old_hbt_id=" << old_id
                                       << " parent_node=" << branch.parent_tree_node << " child_node=" << branch.child_tree_node
                                       << " parent_die=" << branch.parent_point.die << " child_die=" << branch.child_point.die << "\n";
        if (normalizeDieLocal(branch.parent_point.die) == normalizeDieLocal(branch.child_point.die)) {
            if (stats) stats->rejected_by_build_hbt_branch_failed++;
            if (params_.verbose) std::cout << "[NetReRoute][HBT-SWAP][build] net=" << net.name << " old_hbt_id=" << old_id << " reason=same_die\n";
            continue;
        }
        for (int new_id : collectCandidateHBTsForReroute(net, result, branch.parent_point, branch.child_point, old_id, params_.beam_width)) {
            if (new_id == old_id) continue;
            if (stats) stats->tried_hbt_swap_candidates++;
            std::vector<RoutedSegment> segs; std::string fr;
            if (!buildCrossDieBranchViaHBT(branch.parent_point, branch.child_point, new_id, segs, fr)) {
                if (stats) stats->rejected_by_build_hbt_branch_failed++;
                if (params_.verbose) std::cout << "[NetReRoute][HBT-SWAP][build] net=" << net.name << " old_hbt_id=" << old_id << " new_hbt_id=" << new_id << " fail_reason=" << fr << "\n";
                continue;
            }
            if (params_.verbose) std::cout << "[NetReRoute][HBT-SWAP][build] net=" << net.name << " old_hbt_id=" << old_id << " new_hbt_id=" << new_id << " status=ok\n";
            NetEditCandidate c;
            c.type=EditType::kSwapHBT; c.sink_pin_index=sink_pin_index; c.sink_tree_node=branch.child_tree_node;
            c.old_parent_tree_node=branch.parent_tree_node; c.new_parent_tree_node=branch.parent_tree_node;
            c.old_hbt_id=old_id; c.new_hbt_id=new_id; c.old_hbt_ids={old_id}; c.new_hbt_ids={new_id};
            c.old_segments=branch.old_segments; c.new_segments=segs; c.child_attach_point=branch.child_point; c.has_child_attach_point=true;
            c.changed_hbt_id_count=1; c.changed_segment_count=(int)segs.size();
            out.push_back(c);
            if((int)out.size()>=params_.max_iterations_per_net) return out;
        }
    }
    return out;
}
