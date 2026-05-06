#include "NetReRoute2D.h"
#include <iostream>

NetReRoute2DOptimizer::NetReRoute2DOptimizer(const RouterDB&, const HybridGrid&, const PDTreeRouter&, HBTResourceManager&, const CriticalNetOptimizer::Params&) {}

CriticalNetOptimizer::OptimizationStats NetReRoute2DOptimizer::optimize2DNet(const Net& net, NetRouteResult& result, CriticalNetOptimizer* owner, bool fixed_hbt) const {
    CriticalNetOptimizer::OptimizationStats st; std::cout << "[2D] start net=" << net.name << "\n";
    int sink_pin = result.delay_summary.max_delay_pin_index; if (sink_pin < 0) return st;
    int sink_node = -1; for (int i=0;i<(int)result.tree_nodes.size();++i) if(result.tree_nodes[i].pin_index==sink_pin){sink_node=i;break;}
    if (sink_node <= 0) return st;
    int tried=0, acc=0, rej=0;
    for (int p = 0; p < (int)result.tree_nodes.size() && tried < 8; ++p) {
        if (p == sink_node || result.tree_nodes[p].point.die != result.tree_nodes[sink_node].point.die) continue;
        RerouteCandidate c; c.net_name=net.name; c.is_3d=false; c.target_sink_pin_index=sink_pin; c.target_sink_tree_node=sink_node; c.new_parent_tree_node=p;
        c.candidate_type = fixed_hbt ? RerouteCandidateType::k3DIntraDieReattachWithFixedHBT : RerouteCandidateType::k2DReattach;
        RoutedSegment s; s.p1=result.tree_nodes[p].point; s.p2=result.tree_nodes[sink_node].point; s.uses_hbt=false; c.new_segments={s};
        NetRouteResult cand; auto e=owner->evaluateCandidate(net,result,c,&cand); ++tried; ++st.tried_candidates; if (e.accepted) { result = cand; ++acc; ++st.accepted_candidates; st.improved_nets=1; break; } else {++rej; ++st.rejected_candidates;}
    }
    std::cout << "[2D] net="<<net.name<<" step=reattach tried="<<tried<<" accepted="<<acc<<" rejected="<<rej<<"\n";
    std::cout << "[2D] net="<<net.name<<" step=ripup tried=0 accepted=0 rejected=0\n";
    return st;
}
