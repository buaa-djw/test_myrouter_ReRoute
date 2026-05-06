#pragma once
#include "NetReRoute.h"

class NetReRoute2DOptimizer {
public:
    NetReRoute2DOptimizer(const RouterDB& db, const HybridGrid& grid, const PDTreeRouter& router, HBTResourceManager& hbt_manager, const CriticalNetOptimizer::Params& params);
    CriticalNetOptimizer::OptimizationStats optimize2DNet(const Net& net, NetRouteResult& result, CriticalNetOptimizer* owner, bool fixed_hbt) const;
};
