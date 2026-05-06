#include "NetReRoute3D.h"
#include <iostream>

NetReRoute3DOptimizer::NetReRoute3DOptimizer(const RouterDB&, const HybridGrid&, const PDTreeRouter&, HBTResourceManager&, const CriticalNetOptimizer::Params&) {}

CriticalNetOptimizer::OptimizationStats NetReRoute3DOptimizer::optimize3DNet(const Net& net, NetRouteResult& result, CriticalNetOptimizer* owner, const NetReRoute2DOptimizer& d2) const {
    CriticalNetOptimizer::OptimizationStats st; std::cout << "[3D] start net=" << net.name << "\n";
    std::cout << "[3D] step=cross_layer_detour tried=0 accepted=0 rejected=0\n";
    std::cout << "[3D] step=hbt_replace tried=0 accepted=0 rejected=0\n";
    std::cout << "[3D] step=hbt_parent_change tried=0 accepted=0 rejected=0\n";
    std::cout << "[3D] step=hbt_child_change tried=0 accepted=0 rejected=0\n";
    auto inner = d2.optimize2DNet(net, result, owner, true);
    st.tried_candidates += inner.tried_candidates; st.accepted_candidates += inner.accepted_candidates; st.rejected_candidates += inner.rejected_candidates; st.improved_nets += inner.improved_nets;
    std::cout << "[3D] step=intra_die_2d_fixed_hbt tried="<<inner.tried_candidates<<" accepted="<<inner.accepted_candidates<<" rejected="<<inner.rejected_candidates<<"\n";
    return st;
}
