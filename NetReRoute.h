#pragma once

#include "Grid.h"
#include "HBTResourceManager.h"
#include "PDTreeRouter.h"
#include "RouterDB.h"

#include <string>
#include <vector>

class CriticalNetOptimizer
{
public:
    struct Params
    {
        int top_k_nets = 20;
        int max_iterations_per_net = 10;
        double max_wirelength_growth_ratio = 0.05;
        int max_extra_hbts = 0;
        bool enable_reattach = true;
        bool enable_ripup = false;
        bool enable_hbt_swap = false;
        double objective_weight_max_delay = 1.0;
        double objective_weight_avg_delay = 0.2;
        double objective_weight_wirelength_growth = 0.05;
        double objective_weight_hbt_count = 0.1;
        double objective_weight_hbt_delay = 0.2;
        int beam_width = 4;
        std::string target_net_type = "all";
        bool debug_force_accept_hbt_swap = false;
        bool verbose = true;
    };
    struct OptimizationStats
    {
        int visited_nets = 0, improved_nets = 0, tried_candidates = 0, accepted_candidates = 0, rejected_by_topology = 0, rejected_by_delay = 0, rejected_by_wirelength = 0, rejected_by_hbt_conflict = 0;
        int tried_reattach_candidates = 0, tried_ripup_candidates = 0, accepted_reattach_candidates = 0, accepted_ripup_candidates = 0, rejected_by_cycle = 0, rejected_by_cross_die_not_supported = 0, rejected_by_invalid_hbt = 0;
        int tried_hbt_swap_candidates = 0, accepted_hbt_swap_candidates = 0, tried_cross_die_ripup_candidates = 0, accepted_cross_die_ripup_candidates = 0;
        int tried_hbt_insert_candidates = 0, accepted_hbt_insert_candidates = 0, tried_hbt_remove_candidates = 0, accepted_hbt_remove_candidates = 0;
        int rejected_by_no_free_hbt = 0, rejected_by_no_hbt_on_path = 0, rejected_by_non_3d_net = 0, rejected_by_build_hbt_branch_failed = 0, rejected_by_hbt_swap_not_applied = 0;
        int visited_2d_nets = 0, visited_3d_nets = 0;
        int hbt_swap_force_accept_used = 0;
        int changed_hbt_id_count = 0, changed_hbt_count_total = 0;
        int hbt_count_before = 0, hbt_count_after = 0;
        int hbt_conflict_before = 0, hbt_conflict_after = 0;
        double total_avg_delay_before = 0, total_avg_delay_after = 0, total_max_delay_before = 0, total_max_delay_after = 0, total_objective_before = 0, total_objective_after = 0, total_wirelength_before = 0, total_wirelength_after = 0;
        int total_hbt_count_before = 0, total_hbt_count_after = 0;
    };
    struct CriticalNetRecord
    {
        int result_index = -1, net_index = -1;
        std::string net_name;
        double avg_delay = 0, max_delay = 0;
        int max_pin_index = -1;
    };
    enum class EditType
    {
        kReattachSinkSameDie,
        kRipupOneSinkBranch,
        kCrossDieRipupViaHBT,
        kSwapHBT,
        kInsertHBT,
        kRemoveHBT
    };
    struct NetEditCandidate
    {
        EditType type = EditType::kReattachSinkSameDie;
        int sink_pin_index = -1, sink_tree_node = -1, old_parent_tree_node = -1, new_parent_tree_node = -1;
        std::vector<RoutedSegment> old_segments, new_segments;
        std::vector<int> old_hbt_ids, new_hbt_ids;
        int old_hbt_id = -1, new_hbt_id = -1;
        double old_objective = 0, new_objective = 0, old_max_delay = 0, new_max_delay = 0, old_avg_delay = 0, new_avg_delay = 0, old_wirelength = 0, new_wirelength = 0;
        int old_hbt_count = 0, new_hbt_count = 0;
        double old_hbt_delay_contrib = 0, new_hbt_delay_contrib = 0;
        int changed_hbt_id_count = 0, changed_parent_count = 0, changed_segment_count = 0;
        std::string reject_reason;
    };
    CriticalNetOptimizer(const RouterDB &db, const HybridGrid &grid, const PDTreeRouter &router, HBTResourceManager &hbt_manager, const Params &params);
    OptimizationStats optimize(std::vector<NetRouteResult> &results) const;

private:
    std::vector<CriticalNetRecord> collectCriticalNets(const std::vector<NetRouteResult> &results) const;
    bool optimizeOneNet(const Net &net, NetRouteResult &result, OptimizationStats &stats) const;
    int findTreeNodeForPin(const NetRouteResult &result, int pin_index) const;
    bool isDescendantTreeNode(const NetRouteResult &result, int ancestor, int node) const;
    bool wouldCreateCycle(const NetRouteResult &result, int new_parent, int child) const;
    bool replaceSinkIncomingBranch(const Net &net, NetRouteResult &result, int sink_tree_node, int new_parent_tree_index, const std::vector<RoutedSegment> &new_segments, const RoutedPoint &sink_point) const;
    bool rebuildTreeStatistics(const Net &net, NetRouteResult &result) const;
    bool rollbackToSnapshot(NetRouteResult &result, const NetRouteResult &snapshot_result, HBTResourceManager &hbt_manager, const HBTResourceManager::Snapshot &snapshot) const;
    bool applyRipupCandidate(const Net &net, NetRouteResult &result, const NetEditCandidate &cand, HBTResourceManager &hbt_manager, std::string &fail_reason) const;
    bool buildCrossDieBranchViaHBT(const RoutedPoint& parent_point, const RoutedPoint& sink_point, int hbt_id, std::vector<RoutedSegment>& out_segments, std::string& fail_reason) const;
    std::vector<int> collectHBTsOnPath(const NetRouteResult& result, int sink_tree_node) const;
    std::vector<int> collectAllUsedHBTsInNet(const NetRouteResult& result) const;
    std::vector<int> collectCandidateHBTsForReroute(const Net& net, const NetRouteResult& result, const RoutedPoint& parent_point, const RoutedPoint& sink_point, int old_hbt_id, int max_count) const;
    bool verifyHBTSwapApplied(const NetRouteResult& result, int old_hbt_id, int new_hbt_id, std::string& reason) const;
    std::vector<NetEditCandidate> generateHBTSwapCandidates(const Net &net, const NetRouteResult &result, int sink_pin_index, int sink_tree_node, OptimizationStats *stats) const;
    std::vector<NetEditCandidate> generateRipupOneSinkCandidates(const Net &net, const NetRouteResult &result, int sink_pin_index, int sink_tree_node, OptimizationStats *stats) const;
    double evaluatePostOptimizationObjective(const NetRouteResult &result, const Net &net) const;
    const RouterDB &db_;
    const HybridGrid &grid_;
    const PDTreeRouter &router_;
    HBTResourceManager &hbt_manager_;
    Params params_;
};
