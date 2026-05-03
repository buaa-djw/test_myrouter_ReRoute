#pragma once

#include "Grid.h"
#include "PDTreeRouter.h"
#include "RouterDB.h"

#include <string>
#include <vector>

class CriticalNetOptimizer
{
public:
    struct Params {
        int top_k_nets = 20;
        int max_iterations_per_net = 10;
        double max_wirelength_growth_ratio = 0.05;
        int max_extra_hbts = 0;
        bool optimize_for_max_delay = true;
        bool verbose = true;
    };

    struct OptimizationStats {
        int visited_nets = 0;
        int improved_nets = 0;
        double total_avg_delay_before = 0.0;
        double total_avg_delay_after = 0.0;
        double total_max_delay_before = 0.0;
        double total_max_delay_after = 0.0;
    };

    struct CriticalNetRecord {
        int result_index = -1;
        int net_index = -1;
        std::string net_name;
        double avg_delay = 0.0;
        double max_delay = 0.0;
        int max_pin_index = -1;
    };

    enum class EditType {
        kReattachSinkSameDie,
        kSwapHBT,
        kRipupOneSinkBranch
    };

    struct NetEditCandidate {
        EditType type = EditType::kReattachSinkSameDie;
        int target_pin_index = -1;
        int target_tree_node = -1;
        int new_parent_tree_index = -1;
        int new_hbt_id = -1;
    };

public:
    CriticalNetOptimizer(const RouterDB& db,
                         const HybridGrid& grid,
                         const PDTreeRouter& router,
                         const Params& params);

    OptimizationStats optimize(std::vector<NetRouteResult>& results) const;

private:
    std::vector<CriticalNetRecord> collectCriticalNets(const std::vector<NetRouteResult>& results) const;
    bool optimizeOneNet(const Net& net, NetRouteResult& result) const;

    int findTreeNodeForPin(const NetRouteResult& result, int pin_index) const;
    bool isInSubtree(const NetRouteResult& result, int root_node, int query_node) const;
    bool replaceSinkIncomingBranch(const Net& net,
                                   NetRouteResult& result,
                                   int sink_tree_node,
                                   int new_parent_tree_index,
                                   const std::vector<RoutedSegment>& new_segments,
                                   const RoutedPoint& sink_point) const;
    bool rebuildTreeStatistics(const Net& net, NetRouteResult& result) const;

    double evaluatePostOptimizationObjective(const NetRouteResult& result,
                                             const Net& net) const;

    std::vector<NetEditCandidate> generateHBTSwapCandidates(const Net& net,
                                                             const NetRouteResult& result,
                                                             int sink_tree_node) const;

private:
    const RouterDB& db_;
    const HybridGrid& grid_;
    const PDTreeRouter& router_;
    Params params_;
};
