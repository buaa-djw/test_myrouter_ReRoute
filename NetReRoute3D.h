#pragma once
#include "NetReRoute.h"
#include "NetReRoute2D.h"

class NetReRoute3DOptimizer {
public:
    NetReRoute3DOptimizer(const RouterDB& db, const HybridGrid& grid, const PDTreeRouter& router, HBTResourceManager& hbt_manager, const CriticalNetOptimizer::Params& params);
    CriticalNetOptimizer::OptimizationStats optimize3DNet(const Net& net, NetRouteResult& result, CriticalNetOptimizer* owner, const NetReRoute2DOptimizer& d2) const;
};
