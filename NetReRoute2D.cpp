#include "NetReRoute2D.h"
#include <iostream>

NetReRoute2DOptimizer::NetReRoute2DOptimizer(const RouterDB&, const HybridGrid&, const PDTreeRouter&, HBTResourceManager&, const CriticalNetOptimizer::Params&) {}

CriticalNetOptimizer::OptimizationStats NetReRoute2DOptimizer::optimize2DNet(const Net& net, NetRouteResult& result, CriticalNetOptimizer* owner, bool fixed_hbt) const {
    CriticalNetOptimizer::OptimizationStats st; std::cout << "[2D] start net=" << net.name << " fixed_hbt=" << (fixed_hbt?1:0) << "\n";
    if (!owner) return st;
    int sink_pin = result.delay_summary.max_delay_pin_index, sink_node = -1;
    for (int i=0;i<(int)result.tree_nodes.size();++i) if (result.tree_nodes[i].pin_index == sink_pin) { sink_node = i; break; }
    if (sink_node < 0) {
        for (int i=0;i<(int)result.tree_nodes.size();++i) {
            if (result.tree_nodes[i].pin_index >= 0 && result.tree_nodes[i].parent_index >= 0 && result.tree_nodes[i].pin_index != net.source_pin_index) { sink_node=i; sink_pin=result.tree_nodes[i].pin_index; break; }
        }
    }
    if (sink_node < 0) { std::cout << "[2D] net="<<net.name<<" reject reason=no_sink_node\n"; return st; }
    auto isDesc=[&](int node,int maybe_parent){ while(node>=0){ if(node==maybe_parent) return true; node=result.tree_nodes[node].parent_index;} return false; };

    int tried=0, accepted=0;
    for (int p=0;p<(int)result.tree_nodes.size();++p) {
        if (p==sink_node) continue;
        if (result.tree_nodes[p].point.die != result.tree_nodes[sink_node].point.die) continue;
        if (isDesc(p,sink_node)) continue;
        std::string reason;
        ++tried;
        if (owner->trySameDieReattachForModule(net, result, sink_node, sink_pin, p, fixed_hbt, &st, &reason)) { accepted=1; st.improved_nets=1; std::cout << "[2D] net="<<net.name<<" step=reattach accepted parent="<<p<<"\n"; break; }
    }
    std::cout << "[2D] net="<<net.name<<" step=reattach tried="<<tried<<" accepted="<<accepted<<"\n";
    std::cout << "[2D] net="<<net.name<<" step=ripup tried="<<tried<<" accepted="<<accepted<<"\n";
    return st;
}
