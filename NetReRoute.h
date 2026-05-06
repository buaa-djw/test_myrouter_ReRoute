#pragma once

#include "Grid.h"
#include "HBTResourceManager.h"
#include "PDTreeRouter.h"
#include "RouteTopologyValidator.h"
#include "RouterDB.h"
#include <string>
#include <vector>

class NetReRoute2DOptimizer;
class NetReRoute3DOptimizer;

enum class RerouteCandidateType { k2DReattach, k2DRipup, k3DCrossLayerDetour, k3DHBTReplace, k3DHBTParentChange, k3DHBTSinkChildChange, k3DIntraDieReattachWithFixedHBT, k3DIntraDieRipupWithFixedHBT };
struct RerouteCandidate { RerouteCandidateType candidate_type = RerouteCandidateType::k2DReattach; std::string net_name; bool is_3d=false; int target_sink_pin_index=-1; int target_sink_tree_node=-1; int old_parent_tree_node=-1; int new_parent_tree_node=-1; int old_hbt_id=-1; int new_hbt_id=-1; int affected_hbt_id=-1; std::vector<int> inserted_hbt_ids; std::vector<int> removed_hbt_ids; std::vector<RoutedSegment> new_segments; std::string description; std::string reject_reason; };
struct RerouteEvaluation { bool topology_valid=false, complete_routing_valid=false, hbt_valid=false, edcompute_ready=false, delay_improved=false, objective_improved=false, accepted=false; std::string reject_reason; double max_delay_before=0,max_delay_after=0,avg_delay_before=0,avg_delay_after=0,critical_sink_delay_before=0,critical_sink_delay_after=0,wirelength_before=0,wirelength_after=0; int hbt_count_before=0,hbt_count_after=0; double objective_before=0, objective_after=0; };

class CriticalNetOptimizer {
public:
    struct Params {
        int top_k_nets = 20, max_iterations_per_net = 20, max_hbt_candidates_per_branch = 8, beam_width = 4;
        double max_wirelength_growth_ratio = 0.5, objective_weight_max_delay = 1.0, objective_weight_avg_delay = 0.35, objective_weight_critical_sink_delay = 0.9, objective_weight_wirelength = 0.05, objective_weight_hbt_count = 0.08;
        int max_extra_hbts = 1, debug_force_accept_limit = 1, max_candidates_per_net = 64, max_hbt_candidates_per_sink = 8, detour_search_radius = 200, debug_force_accepted_count = 0;
        double max_pathlength_growth_ratio = 1.5;
        bool enable=true, enable_2d_optimization=true, enable_3d_optimization=true, enable_2d_reattach=true, enable_2d_ripup=true, enable_3d_cross_layer_detour=true, enable_3d_hbt_replace=true, enable_3d_hbt_parent_change=true, enable_3d_hbt_child_change=true, enable_3d_intra_die_optimization=true, debug_force_accept=false, verbose=true, dump_candidate_csv=true;
    };
    struct OptimizationStats { int visited_nets=0, improved_nets=0, tried_candidates=0, accepted_candidates=0, rejected_candidates=0, rejected_by_topology=0, rejected_by_delay=0, rejected_by_wirelength=0, rejected_by_hbt_conflict=0, hbt_conflict_before=0, hbt_conflict_after=0, tried_cross_die_ripup_candidates=0, accepted_cross_die_ripup_candidates=0, rejected_by_no_free_hbt=0, rejected_by_cross_die_not_supported=0, rejected_by_build_hbt_branch_failed=0, force_accepted_cross_die_ripup_candidates=0;};

    CriticalNetOptimizer(const RouterDB&, const HybridGrid&, const PDTreeRouter&, HBTResourceManager&, const Params&);
    OptimizationStats optimize(std::vector<NetRouteResult>& results) const;
    RerouteEvaluation evaluateCandidate(const Net& net, const NetRouteResult& base_result, const RerouteCandidate& candidate, NetRouteResult* candidate_result) const;

private:
    const RouterDB& db_; const HybridGrid& grid_; const PDTreeRouter& router_; HBTResourceManager& hbt_manager_; Params params_; RouteTopologyValidator validator_;
};
