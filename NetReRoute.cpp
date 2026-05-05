#include "NetReRoute.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <queue>
#include <unordered_map>

namespace
{
    DieId normalizeDieLocal(DieId d) { return (d == DieId::kTop || d == DieId::kBottom) ? d : DieId::kTop; }
    constexpr const char* kReasonInvalidTopology = "invalid_topology";
    constexpr const char* kReasonHbtReserveConflict = "hbt_reserve_conflict";
    constexpr const char* kReasonSwapNotApplied = "hbt_swap_not_reflected_in_topology";
    constexpr const char* kReasonInsertNotApplied = "invalid_or_unchanged_hbt_id";
    constexpr const char* kReasonRemoveNotApplied = "removed_hbt_still_exists";

    const char* dieIdToStringLocal(DieId d)
    {
        switch (d) {
        case DieId::kTop:
            return "Top";
        case DieId::kBottom:
            return "Bottom";
        case DieId::kUnknown:
        default:
            return "Unknown";
        }
    }
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
    result.reroute_info = RerouteInfo{};
    if (!result.success || !result.delay_summary.ready || result.delay_summary.single_pin_net) {
        return false;
    }

    const int sink_pin = result.delay_summary.max_delay_pin_index;
    const int sink_tree = findTreeNodeForPin(result, sink_pin);
    if (sink_tree < 0 || sink_tree == result.root_tree_index) {
        return false;
    }

    const double base_obj = evaluatePostOptimizationObjective(result, net);
    const double base_max_delay = result.delay_summary.max_sink_delay;
    const double base_avg_delay = result.delay_summary.avg_sink_delay;
    const double base_hbt_delay = result.delay_summary.ed_hbt_delay_contrib;
    const auto base_t = router_.evaluateTimingSummaryPublic(net, result);

    std::vector<NetEditCandidate> cands;

    if (params_.enable_reattach) {
        for (int p : router_.collectCandidateParentsPublic(net, result, net.pins[sink_pin])) {
            if (static_cast<int>(cands.size()) >= params_.max_iterations_per_net) {
                break;
            }
            if (p == sink_tree || wouldCreateCycle(result, p, sink_tree)) {
                stats.rejected_by_cycle++;
                continue;
            }

            std::vector<RoutedSegment> segs;
            const RoutedPoint parent_point = result.tree_nodes[p].point;
            const RoutedPoint sink_point = router_.pinToPointPublic(net.pins[sink_pin]);
            if (normalizeDieLocal(parent_point.die) != normalizeDieLocal(sink_point.die)) {
                stats.rejected_by_cross_die_not_supported++;
                continue;
            }
            if (!router_.build2DConnectionPublic(parent_point, sink_point, segs)) {
                continue;
            }

            NetEditCandidate c;
            c.type = EditType::kReattachSameDie;
            c.sink_pin_index = sink_pin;
            c.sink_tree_node = sink_tree;
            c.old_parent_tree_node = result.tree_nodes[sink_tree].parent_index;
            c.new_parent_tree_node = p;
            c.new_segments = segs;
            cands.push_back(c);
        }
    }

    if (params_.enable_ripup) {
        auto ripup_candidates = generateRipupOneSinkCandidates(net, result, sink_pin, sink_tree, &stats);
        cands.insert(cands.end(), ripup_candidates.begin(), ripup_candidates.end());
    }

    if (params_.enable_hbt_swap) {
        auto hbt_candidates = generateHBTSwapCandidates(net, result, sink_pin, sink_tree, &stats);
        cands.insert(cands.end(), hbt_candidates.begin(), hbt_candidates.end());
    }
    if (params_.enable_hbt_insert) {
        auto v = generateHBTInsertCandidates(net, result, sink_pin, sink_tree, &stats);
        cands.insert(cands.end(), v.begin(), v.end());
    }
    if (params_.enable_hbt_remove) {
        auto v = generateHBTRemoveCandidates(net, result, sink_pin, sink_tree, &stats);
        cands.insert(cands.end(), v.begin(), v.end());
    }

    if (cands.empty()) {
        return false;
    }

    NetRouteResult best_result = result;
    NetEditCandidate bestc;
    double best_obj = base_obj;
    bool found = false;
    bool best_force_accepted = false;
    auto accountReject = [&](const NetEditCandidate& c, const std::string& reason, bool force_mode) {
        if (force_mode) stats.rejected_by_force_verify_failed++;
        if (reason == kReasonInvalidTopology) stats.rejected_by_topology++;
        else if (reason == kReasonHbtReserveConflict) stats.rejected_by_hbt_conflict++;
        else if (reason == kReasonSwapNotApplied) stats.rejected_by_hbt_swap_not_applied++;
        else if (reason == kReasonRemoveNotApplied) stats.rejected_by_hbt_remove_not_applied++;
        else if (reason == kReasonInsertNotApplied && c.type == EditType::kInsertHBT) stats.rejected_by_hbt_insert_not_applied++;
    };

    for (auto &c : cands) {
        stats.tried_candidates++;
        if (c.type == EditType::kRipupOneSink) {
            stats.tried_ripup_candidates++;
        } else if (c.type == EditType::kReattachSameDie) {
            stats.tried_reattach_candidates++;
        } else if (c.type == EditType::kCrossDieRipup) {
            stats.tried_cross_die_ripup_candidates++;
        } else if (c.type == EditType::kInsertHBT) {
            stats.tried_hbt_insert_candidates++;
        } else if (c.type == EditType::kRemoveHBT) {
            stats.tried_hbt_remove_candidates++;
        }

        const bool force_mode =
            (c.type == EditType::kSwapHBT && params_.debug_force_accept_hbt_swap) ||
            (c.type == EditType::kInsertHBT && params_.debug_force_accept_hbt_insert) ||
            (c.type == EditType::kRemoveHBT && params_.debug_force_accept_hbt_remove);
        const CandidateEvaluation ev = evaluateCandidate(net, result, c);
        const double cand_obj = ev.objective_after;
        if (!ev.apply_ok) {
            c.reject_reason = ev.reject_reason;
            accountReject(c, ev.reject_reason, force_mode);
            continue;
        }

        if (params_.verbose && c.type == EditType::kSwapHBT) {
            std::cout << "[NetReRoute][HBT-SWAP][candidate] net=" << net.name
                      << " old_hbt=" << c.old_hbt_id
                      << " new_hbt=" << c.new_hbt_id
                      << " old_wl=" << base_t.total_wirelength
                      << " new_wl=" << cand_timing.total_wirelength
                      << " old_delay=" << snap.delay_summary.max_sink_delay
                      << " new_delay=" << result.delay_summary.max_sink_delay
                      << " old_obj=" << base_obj
                      << " new_obj=" << cand_obj << "\n";
        }

        if (force_mode) {
            bool force_ok = ev.apply_ok && ev.topology_ok && ev.hbt_ok && ev.delay_ready && ev.hbt_change_ok;
            std::string verify_reason = ev.reject_reason.empty() ? "force_verify_failed" : ev.reject_reason;

            if (force_ok) {
                found = true;
                best_result = result;
                bestc = c;
                best_obj = cand_obj;
                best_force_accepted = true;
                stats.hbt_swap_force_accept_used = 1;
                if (c.type == EditType::kInsertHBT) stats.hbt_insert_force_accept_used++;
                if (c.type == EditType::kRemoveHBT) stats.hbt_remove_force_accept_used++;
                if (params_.verbose) {
                    std::cout << "[NetReRoute][accept-candidate] net=" << net.name
                              << " type=hbt_swap old_hbt=" << c.old_hbt_id
                              << " new_hbt=" << c.new_hbt_id
                              << " force=1 obj_before=" << base_obj
                              << " obj_after=" << cand_obj
                              << " delay_before=" << base_max_delay
                              << " delay_after=" << result.delay_summary.max_sink_delay << "\n";
                }
                break;
            }

            stats.rejected_by_force_verify_failed++;
            accountReject(c, verify_reason, true);
            if (params_.verbose) {
                std::cout << "[NetReRoute][reject] net=" << net.name
                          << " type=hbt_swap old_hbt=" << c.old_hbt_id
                          << " new_hbt=" << c.new_hbt_id
                          << " reason=" << verify_reason
                          << " obj_before=" << base_obj
                          << " obj_after=" << cand_obj
                          << " wl_before=" << ev.wirelength_before
                          << " wl_after=" << ev.wirelength_after
                          << " delay_before=" << base_max_delay
                          << " delay_after=" << ev.max_delay_after << "\n";
            }
            continue;
        }

        const bool wl_ok = ev.wirelength_after <= ev.wirelength_before * (1.0 + params_.max_wirelength_growth_ratio) + 1e-9;
        const bool hbt_ok = ev.hbt_count_after <= ev.hbt_count_before + params_.max_extra_hbts;
        const bool delay_ok = ev.delay_ready && ev.max_delay_after <= ev.max_delay_before + 1e-12;

        if (!wl_ok) {
            stats.rejected_by_wirelength++;
            if (params_.verbose && c.type == EditType::kSwapHBT) {
                std::cout << "[NetReRoute][reject] net=" << net.name
                          << " type=hbt_swap old_hbt=" << c.old_hbt_id
                          << " new_hbt=" << c.new_hbt_id
                          << " reason=wirelength obj_before=" << base_obj
                          << " obj_after=" << cand_obj
                          << " wl_before=" << ev.wirelength_before
                          << " wl_after=" << ev.wirelength_after
                          << " delay_before=" << base_max_delay
                          << " delay_after=" << ev.max_delay_after << "\n";
            }
            continue;
        }
        if (!hbt_ok) {
            stats.rejected_by_hbt_conflict++;
            if (params_.verbose && c.type == EditType::kSwapHBT) {
                std::cout << "[NetReRoute][reject] net=" << net.name
                          << " type=hbt_swap old_hbt=" << c.old_hbt_id
                          << " new_hbt=" << c.new_hbt_id
                          << " reason=hbt_count obj_before=" << base_obj
                          << " obj_after=" << cand_obj << "\n";
            }
            continue;
        }
        if (!delay_ok) {
            stats.rejected_by_delay++;
            if (params_.verbose && c.type == EditType::kSwapHBT) {
                std::cout << "[NetReRoute][reject] net=" << net.name
                          << " type=hbt_swap old_hbt=" << c.old_hbt_id
                          << " new_hbt=" << c.new_hbt_id
                          << " reason=delay obj_before=" << base_obj
                          << " obj_after=" << cand_obj
                          << " wl_before=" << ev.wirelength_before
                          << " wl_after=" << ev.wirelength_after
                          << " delay_before=" << base_max_delay
                          << " delay_after=" << ev.max_delay_after << "\n";
            }
            continue;
        }
        if (!(cand_obj + 1e-12 < best_obj)) {
            stats.rejected_by_no_objective_improvement++;
            if (params_.verbose && c.type == EditType::kSwapHBT) {
                std::cout << "[NetReRoute][reject] net=" << net.name
                          << " type=hbt_swap old_hbt=" << c.old_hbt_id
                          << " new_hbt=" << c.new_hbt_id
                          << " reason=no_objective_improvement obj_before=" << best_obj
                          << " obj_after=" << cand_obj << "\n";
            }
            continue;
        }

        found = true;
        best_result = result;
        bestc = c;
        best_obj = cand_obj;
        best_force_accepted = false;
    }

    if (!found) {
        return false;
    }

    std::string final_fail;
    if (!applyCandidateToResult(net, result, bestc, hbt_manager_, final_fail)) {
        if (best_force_accepted) {
            stats.rejected_by_force_verify_failed++;
        }
        if (params_.verbose) {
            std::cout << "[NetReRoute][reject] net=" << net.name
                      << " final_apply_failed reason=" << final_fail << "\n";
        }
        return false;
    }

    if (bestc.type == EditType::kSwapHBT) {
        std::string verify_reason;
        if (!verifyHBTSwapApplied(result, bestc.old_hbt_id, bestc.new_hbt_id, verify_reason)) {
            stats.rejected_by_hbt_swap_not_applied++;
            if (params_.verbose) {
                std::cout << "[NetReRoute][reject] net=" << net.name
                          << " final_verify_failed reason=" << verify_reason << "\n";
            }
            return false;
        }
    }

    stats.accepted_candidates++;
    if (bestc.type == EditType::kRipupOneSink) {
        stats.accepted_ripup_candidates++;
    } else if (bestc.type == EditType::kReattachSameDie) {
        stats.accepted_reattach_candidates++;
    } else if (bestc.type == EditType::kSwapHBT) {
        stats.accepted_hbt_swap_candidates++;
        stats.swapped_hbt_count++;
    } else if (bestc.type == EditType::kCrossDieRipup) {
        stats.accepted_cross_die_ripup_candidates++;
    } else if (bestc.type == EditType::kInsertHBT) {
        stats.accepted_hbt_insert_candidates++;
        stats.inserted_hbt_count++;
    } else if (bestc.type == EditType::kRemoveHBT) {
        stats.accepted_hbt_remove_candidates++;
        stats.removed_hbt_count++;
    }

    stats.changed_hbt_id_count += bestc.changed_hbt_id_count;
    stats.changed_hbt_count_total += (bestc.new_hbt_count - bestc.old_hbt_count);
    stats.improved_nets++;
    if (best_force_accepted) {
        stats.hbt_swap_force_accept_used = 1;
    }

    const auto final_timing = router_.evaluateTimingSummaryPublic(net, result);
    result.reroute_info.touched = true;
    result.reroute_info.improved = (best_obj + 1e-12 < base_obj);
    result.reroute_info.force_accepted = best_force_accepted;
    result.reroute_info.edit_type =
        (bestc.type == EditType::kRipupOneSink) ? "ripup_one_sink" :
        (bestc.type == EditType::kSwapHBT) ? "hbt_swap" :
        (bestc.type == EditType::kCrossDieRipup) ? "cross_die_ripup" :
        (bestc.type == EditType::kInsertHBT) ? "hbt_insert" :
        (bestc.type == EditType::kRemoveHBT) ? "hbt_remove" :
        "reattach_same_die";
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
    result.reroute_info.inserted_hbt_id = bestc.inserted_hbt_id;
    result.reroute_info.removed_hbt_id = bestc.removed_hbt_id;
    result.reroute_info.changed_hbt_count_delta = bestc.changed_hbt_count_delta;
    result.reroute_info.old_hbt_ids = bestc.old_hbt_ids;
    result.reroute_info.new_hbt_ids = bestc.new_hbt_ids;
    result.reroute_info.inserted_hbt_ids = bestc.inserted_hbt_ids;
    result.reroute_info.removed_hbt_ids = bestc.removed_hbt_ids;
    result.reroute_info.changed_hbt_id_count = bestc.changed_hbt_id_count;
    result.reroute_info.changed_segment_count = bestc.changed_segment_count;
    result.reroute_info.max_delay_before = base_max_delay;
    result.reroute_info.max_delay_after = result.delay_summary.max_sink_delay;
    result.reroute_info.avg_delay_before = base_avg_delay;
    result.reroute_info.avg_delay_after = result.delay_summary.avg_sink_delay;
    result.reroute_info.hbt_delay_before = base_hbt_delay;
    result.reroute_info.hbt_delay_after = result.delay_summary.ed_hbt_delay_contrib;
    result.reroute_info.reject_reason.clear();

    if (bestc.type == EditType::kSwapHBT) {
        std::cout << "[NetReRoute][accept] net=" << net.name
                  << " type=hbt_swap old_hbt=" << bestc.old_hbt_id
                  << " new_hbt=" << bestc.new_hbt_id
                  << " force=" << (best_force_accepted ? 1 : 0)
                  << " obj_before=" << base_obj
                  << " obj_after=" << best_obj
                  << " delay_before=" << base_max_delay
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
        c.type = EditType::kRipupOneSink;
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
bool CriticalNetOptimizer::applyCandidateToResult(const Net &net, NetRouteResult &result, const NetEditCandidate &c, HBTResourceManager &hm, std::string &fr) const
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

CriticalNetOptimizer::CandidateEvaluation CriticalNetOptimizer::evaluateCandidate(
    const Net& net, const NetRouteResult& original, const NetEditCandidate& candidate) const
{
    CandidateEvaluation ev;
    ev.objective_before = evaluatePostOptimizationObjective(original, net);
    ev.delay_before = original.delay_summary.max_sink_delay;
    ev.max_delay_before = original.delay_summary.max_sink_delay;
    ev.avg_delay_before = original.delay_summary.avg_sink_delay;
    ev.hbt_delay_before = original.delay_summary.ed_hbt_delay_contrib;
    const auto before_t = router_.evaluateTimingSummaryPublic(net, original);
    ev.wirelength_before = before_t.total_wirelength;
    ev.hbt_count_before = before_t.hbt_count;

    NetRouteResult work = original;
    auto hs = hbt_manager_.makeSnapshot();
    std::string reason;
    ev.apply_ok = applyCandidateToResult(net, work, candidate, hbt_manager_, reason);
    if (!ev.apply_ok) { ev.reject_reason = reason.empty() ? "apply_failed" : reason; hbt_manager_.rollback(hs); return ev; }
    ev.topology_ok = work.validation.valid;
    ev.delay_ready = work.delay_summary.ready;
    ev.objective_after = evaluatePostOptimizationObjective(work, net);
    ev.delay_after = work.delay_summary.max_sink_delay;
    ev.max_delay_after = work.delay_summary.max_sink_delay;
    ev.avg_delay_after = work.delay_summary.avg_sink_delay;
    ev.hbt_delay_after = work.delay_summary.ed_hbt_delay_contrib;
    const auto after_t = router_.evaluateTimingSummaryPublic(net, work);
    ev.wirelength_after = after_t.total_wirelength;
    ev.hbt_count_after = after_t.hbt_count;
    ev.hbt_ok = (hbt_manager_.collectStats().conflict_count == 0);
    std::string vr;
    if (candidate.type == EditType::kSwapHBT) ev.hbt_change_ok = verifyHBTSwapApplied(work, candidate.old_hbt_id, candidate.new_hbt_id, vr);
    else if (candidate.type == EditType::kInsertHBT) ev.hbt_change_ok = verifyHBTInsertApplied(work, candidate.inserted_hbt_id, vr);
    else if (candidate.type == EditType::kRemoveHBT) ev.hbt_change_ok = verifyHBTRemoveApplied(work, candidate.removed_hbt_id, vr);
    else ev.hbt_change_ok = true;
    if (!ev.hbt_change_ok) ev.reject_reason = vr;
    hbt_manager_.rollback(hs);
    return ev;
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
    if (new_hbt_id < 0 || new_hbt_id == old_hbt_id) {
        reason = "invalid_or_unchanged_hbt_id";
        return false;
    }

    bool has_new_segment = false;
    bool has_new_node = false;
    for (const auto& s : result.segments) {
        if (s.uses_hbt && s.hbt_id == new_hbt_id) {
            has_new_segment = true;
            break;
        }
    }
    for (const auto& n : result.tree_nodes) {
        if (n.assigned_hbt_id == new_hbt_id) {
            has_new_node = true;
            break;
        }
    }

    // A valid swap must be reflected in the routed topology.  Segment-level
    // evidence is the most important because the plot data and EDCompute walk
    // the segment list.  The tree-node marker is expected to be updated by
    // replaceSinkIncomingBranch(), but we accept either representation to avoid
    // falsely rejecting a valid segment update in older intermediate states.
    if (!has_new_segment && !has_new_node) {
        reason = "hbt_swap_not_reflected_in_topology";
        return false;
    }
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
                                       << " parent_die=" << dieIdToStringLocal(branch.parent_point.die) << " child_die=" << dieIdToStringLocal(branch.child_point.die) << "\n";
        if (normalizeDieLocal(branch.parent_point.die) == normalizeDieLocal(branch.child_point.die)) {
            if (stats) stats->rejected_by_build_hbt_branch_failed++;
            if (params_.verbose) std::cout << "[NetReRoute][HBT-SWAP][build] net=" << net.name << " old_hbt_id=" << old_id << " reason=same_die\n";
            continue;
        }
        std::vector<std::pair<double, NetEditCandidate>> scored;
        for (int new_id : collectCandidateHBTsForReroute(net, result, branch.parent_point, branch.child_point, old_id, params_.max_hbt_swap_candidates_per_branch)) {
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
            double old_wl=0,new_wl=0;
            for (const auto& s: branch.old_segments) old_wl += std::abs(s.p1.x-s.p2.x)+std::abs(s.p1.y-s.p2.y);
            for (const auto& s: segs) new_wl += std::abs(s.p1.x-s.p2.x)+std::abs(s.p1.y-s.p2.y);
            c.predicted_wirelength_before = old_wl;
            c.predicted_wirelength_after = new_wl;
            c.predicted_gain = old_wl - new_wl;
            if (c.predicted_gain <= params_.min_predicted_gain_for_hbt_swap && !params_.debug_force_accept_hbt_swap) { if (stats) stats->rejected_by_no_predicted_gain++; continue; }
            scored.push_back({c.predicted_gain, c});
        }
        std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b){ return a.first > b.first; });
        for (const auto& kv : scored) {
            out.push_back(kv.second);
            if ((int)out.size() >= params_.max_hbt_swap_candidates_per_branch) break;
        }
    }
    return out;
}

std::vector<CriticalNetOptimizer::NetEditCandidate> CriticalNetOptimizer::generateHBTInsertCandidates(const Net &net, const NetRouteResult &result, int sink_pin_index, int sink_tree_node, OptimizationStats *stats) const {
    std::vector<NetEditCandidate> out;
    if (!result.delay_summary.ready) return out;
    const auto& child = result.tree_nodes[sink_tree_node];
    int parent_idx = child.parent_index;
    if (parent_idx < 0) return out;
    RoutedPoint pp = result.tree_nodes[parent_idx].point;
    RoutedPoint cp = child.point;
    const bool cross_die = normalizeDieLocal(pp.die) != normalizeDieLocal(cp.die);
    if (!cross_die && !params_.allow_same_die_hbt_detour) return out;
    for (const auto& s : result.segments) {
        if (s.uses_hbt && s.hbt_id >= 0 &&
            ((s.p1.x==pp.x&&s.p1.y==pp.y)||(s.p2.x==cp.x&&s.p2.y==cp.y))) return out;
    }
    auto ids = collectCandidateHBTsForReroute(net, result, pp, cp, -1, params_.max_hbt_insert_candidates_per_branch);
    std::vector<std::pair<double, NetEditCandidate>> scored;
    for (int hid : ids) {
        std::vector<RoutedSegment> segs;
        std::string fr;
        if (cross_die) {
            if (!buildCrossDieBranchViaHBT(pp, cp, hid, segs, fr)) continue;
        } else {
            Point2D hp; if (!grid_.getHBTPoint(hid, hp)) continue;
            RoutedPoint p1{hp.x,hp.y,pp.die}, p2{hp.x,hp.y,cp.die};
            std::vector<RoutedSegment> a,b;
            if (!router_.build2DConnectionPublic(pp,p1,a) || !router_.build2DConnectionPublic(p2,cp,b)) continue;
            segs = a; segs.push_back(RoutedSegment{p1,p2,-1,true,hid}); segs.insert(segs.end(), b.begin(), b.end());
        }
        NetEditCandidate c; c.type=EditType::kInsertHBT; c.sink_pin_index=sink_pin_index; c.sink_tree_node=sink_tree_node;
        c.old_parent_tree_node=parent_idx; c.new_parent_tree_node=parent_idx; c.new_segments=segs; c.old_hbt_count=0; c.new_hbt_count=1;
        c.inserted_hbt_id=hid; c.inserted_hbt_ids={hid}; c.new_hbt_id=hid; c.new_hbt_ids={hid}; c.changed_hbt_count_delta=1; c.changed_hbt_id_count=1;
        double old_wl=0,new_wl=0;
        for (int i=0;i<child.incoming_segment_count;++i){ const auto& s=result.segments[child.incoming_segment_begin+i]; old_wl+=std::abs(s.p1.x-s.p2.x)+std::abs(s.p1.y-s.p2.y);}
        for (const auto& s: segs) new_wl += std::abs(s.p1.x-s.p2.x)+std::abs(s.p1.y-s.p2.y);
        c.predicted_wirelength_before = old_wl; c.predicted_wirelength_after = new_wl; c.predicted_gain = old_wl - new_wl;
        if (c.predicted_gain <= params_.min_predicted_gain_for_hbt_insert && !params_.debug_force_accept_hbt_insert) { if (stats) stats->rejected_by_no_predicted_gain++; continue; }
        scored.push_back({c.predicted_gain, c});
    }
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b){ return a.first > b.first; });
    for (const auto& kv : scored) {
        out.push_back(kv.second);
        if ((int)out.size() >= params_.max_hbt_insert_candidates_per_branch) break;
    }
    return out;
}

std::vector<CriticalNetOptimizer::NetEditCandidate> CriticalNetOptimizer::generateHBTRemoveCandidates(const Net &net, const NetRouteResult &result, int sink_pin_index, int sink_tree_node, OptimizationStats *stats) const {
    std::vector<NetEditCandidate> out;
    (void)sink_pin_index; (void)sink_tree_node;
    auto used = collectAllUsedHBTsInNet(result);
    for (int old_id : used) {
        HBTBranchRef ref;
        if (!findBranchByHBTId(result, old_id, ref)) continue;
        const bool cross_die = normalizeDieLocal(ref.parent_point.die) != normalizeDieLocal(ref.child_point.die);
        if (cross_die) { if (stats) stats->rejected_by_cross_die_requires_hbt++; continue; }
        std::vector<RoutedSegment> segs;
        if (!router_.build2DConnectionPublic(ref.parent_point, ref.child_point, segs)) continue;
        NetEditCandidate c; c.type=EditType::kRemoveHBT; c.sink_pin_index=result.tree_nodes[ref.child_tree_node].pin_index; c.sink_tree_node=ref.child_tree_node;
        c.old_parent_tree_node=ref.parent_tree_node; c.new_parent_tree_node=ref.parent_tree_node; c.old_segments=ref.old_segments; c.new_segments=segs;
        c.removed_hbt_id=old_id; c.removed_hbt_ids={old_id}; c.old_hbt_id=old_id; c.old_hbt_ids={old_id}; c.changed_hbt_count_delta=-1; c.changed_hbt_id_count=1;
        double old_wl=0,new_wl=0;
        for (const auto& s: ref.old_segments) old_wl += std::abs(s.p1.x-s.p2.x)+std::abs(s.p1.y-s.p2.y);
        for (const auto& s: segs) new_wl += std::abs(s.p1.x-s.p2.x)+std::abs(s.p1.y-s.p2.y);
        c.predicted_wirelength_before = old_wl; c.predicted_wirelength_after = new_wl; c.predicted_gain = old_wl - new_wl;
        if (c.predicted_gain <= params_.min_predicted_gain_for_hbt_remove && !params_.debug_force_accept_hbt_remove) { if (stats) stats->rejected_by_no_predicted_gain++; continue; }
        out.push_back(c);
        if ((int)out.size()>=params_.max_hbt_remove_candidates_per_branch) break;
    }
    return out;
}

bool CriticalNetOptimizer::verifyHBTInsertApplied(const NetRouteResult& result, int inserted_hbt_id, std::string& reason) const {
    return verifyHBTSwapApplied(result, -1, inserted_hbt_id, reason);
}
bool CriticalNetOptimizer::verifyHBTRemoveApplied(const NetRouteResult& result, int removed_hbt_id, std::string& reason) const {
    for (const auto& s: result.segments) if (s.uses_hbt && s.hbt_id==removed_hbt_id) { reason="removed_hbt_still_exists"; return false; }
    return true;
}
