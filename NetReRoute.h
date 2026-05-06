#pragma once

#include "Grid.h"
#include "HBTResourceManager.h"
#include "PDTreeRouter.h"
#include "RouterDB.h"

#include <string>
#include <vector>

class CriticalNetOptimizer {
public:
    enum class EditType { kEdgeRelocation, kRipupOneSink, kHBTSwap, kHBTInsert, kHBTRemove, kCrossDieRipupViaHBT };
    struct NetEditCandidate {
        EditType type = EditType::kRipupOneSink;
        int sink_pin_index = -1;
        int sink_tree_node = -1;
        int old_parent_tree_node = -1;
        int new_parent_tree_node = -1;
        int new_hbt_id = -1;
        std::vector<int> new_hbt_ids;
        std::vector<int> inserted_hbt_ids;
        bool has_child_attach_point = false;
        RoutedPoint child_attach_point;
        std::vector<RoutedSegment> new_segments;
        std::string reject_reason;
    };
    struct Params {
        int top_k_nets = 20;
        int max_iterations_per_net = 20;
        int max_hbt_candidates_per_branch = 8;
        double max_wirelength_growth_ratio = 0.05;
        int max_extra_hbts = 1;
        bool enable_edge_relocation = true;
        bool enable_reattach = true;
        bool enable_ripup = true;
        bool enable_hbt_swap = true;
        bool enable_hbt_insert = true;
        bool enable_hbt_remove = true;
        bool enable_cross_die_ripup = false;
        bool enable_cross_die_detour = true;
        int beam_width = 4;
        double objective_weight_wirelength_growth = 0.05;
        double objective_weight_hbt_delay = 0.0;
        bool debug_force_accept_cross_die_ripup = false;
        bool debug_force_accept_cross_die_detour = false;
        double objective_weight_max_delay = 1.0;
        double objective_weight_avg_delay = 0.35;
        double objective_weight_critical_sink_delay = 0.9;
        double objective_weight_wirelength = 0.05;
        double objective_weight_hbt_count = 0.08;
        bool verbose = true;
        bool debug = false;
    };

    struct OptimizationStats {
        int visited_nets = 0, improved_nets = 0, tried_candidates = 0, accepted_candidates = 0;
        int tried_edge_relocation = 0, accepted_edge_relocation = 0;
        int tried_ripup = 0, accepted_ripup = 0;
        int tried_hbt_swap = 0, accepted_hbt_swap = 0;
        int tried_hbt_insert = 0, accepted_hbt_insert = 0;
        int tried_hbt_remove = 0, accepted_hbt_remove = 0;
        int tried_cross_die_ripup_candidates = 0, accepted_cross_die_ripup_candidates = 0;
        int rejected_by_topology = 0, rejected_by_delay = 0, rejected_by_wirelength = 0, rejected_by_hbt_conflict = 0;
        int rejected_by_no_free_hbt = 0, rejected_by_no_better_parent = 0, rejected_by_no_delay_gain = 0;
        int rejected_by_cross_die_not_supported = 0, rejected_by_build_hbt_branch_failed = 0;
        int force_accepted_cross_die_ripup_candidates = 0;
        int hbt_conflict_before = 0, hbt_conflict_after = 0;
    };

    CriticalNetOptimizer(const RouterDB&, const HybridGrid&, const PDTreeRouter&, HBTResourceManager&, const Params&);
    OptimizationStats optimize(std::vector<NetRouteResult>& results) const;
private:
    std::vector<NetEditCandidate> generateCrossDieDetourCandidates(
        const Net& net,
        const NetRouteResult& result,
        int sink_pin_index,
        int sink_tree_node,
        OptimizationStats* stats) const;
    const RouterDB& db_; const HybridGrid& grid_; const PDTreeRouter& router_; HBTResourceManager& hbt_manager_; Params params_;
};
