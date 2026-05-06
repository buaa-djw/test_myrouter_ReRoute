#include "NetReRoute3D.h"
#include <iostream>

NetReRoute3DOptimizer::NetReRoute3DOptimizer(const RouterDB&, const HybridGrid&, const PDTreeRouter&, HBTResourceManager&, const CriticalNetOptimizer::Params&) {}

CriticalNetOptimizer::OptimizationStats NetReRoute3DOptimizer::optimize3DNet(const Net& net, NetRouteResult& result, CriticalNetOptimizer* owner, const NetReRoute2DOptimizer& d2) const {
    CriticalNetOptimizer::OptimizationStats st; std::cout << "[3D] start net=" << net.name << "\n";
    if (!owner) return st;

    auto s1 = owner->runCrossDieDetourForModule(net, result);
    st.tried_candidates += s1.tried_candidates; st.accepted_candidates += s1.accepted_candidates; st.rejected_candidates += s1.rejected_candidates; st.improved_nets += s1.improved_nets;
    std::cout << "[3D] net="<<net.name<<" step=cross_layer_detour tried="<<s1.tried_candidates<<" accepted="<<s1.accepted_candidates<<"\n";

    auto s2 = owner->runHBTSwapForModule(net, result);
    st.tried_candidates += s2.tried_candidates; st.accepted_candidates += s2.accepted_candidates; st.rejected_candidates += s2.rejected_candidates; st.improved_nets += s2.improved_nets;
    std::cout << "[3D] net="<<net.name<<" step=hbt_replace tried="<<s2.tried_candidates<<" accepted="<<s2.accepted_candidates<<"\n";

    std::cout << "[3D] step=hbt_parent_change tried=0 accepted=0 reason=not_implemented_safely\n";

    auto inner = d2.optimize2DNet(net, result, owner, true);
    st.tried_candidates += inner.tried_candidates; st.accepted_candidates += inner.accepted_candidates; st.rejected_candidates += inner.rejected_candidates; st.improved_nets += inner.improved_nets;
    std::cout << "[3D] net="<<net.name<<" step=intra_die_2d_fixed_hbt tried="<<inner.tried_candidates<<" accepted="<<inner.accepted_candidates<<"\n";
    return st;
}
