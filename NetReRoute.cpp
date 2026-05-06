#include "NetReRoute.h"
#include "NetReRoute2D.h"
#include "NetReRoute3D.h"
#include <algorithm>
#include <fstream>
#include <unordered_map>

namespace {
int treeNodeForPin(const NetRouteResult&r,int pin){for(int i=0;i<(int)r.tree_nodes.size();++i)if(r.tree_nodes[i].pin_index==pin)return i;return -1;}
double objective(const CriticalNetOptimizer::Params& p, const PDTreeRouter::TimingSummary& t, double wl, int hbt){ return p.objective_weight_max_delay*t.max_sink_delay + p.objective_weight_avg_delay*t.avg_sink_delay + p.objective_weight_wirelength*wl + p.objective_weight_hbt_count*hbt; }
}

CriticalNetOptimizer::CriticalNetOptimizer(const RouterDB&d,const HybridGrid&g,const PDTreeRouter&r,HBTResourceManager&h,const Params&p):db_(d),grid_(g),router_(r),hbt_manager_(h),params_(p){}

RerouteEvaluation CriticalNetOptimizer::evaluateCandidate(const Net& net, const NetRouteResult& base_result, const RerouteCandidate& candidate, NetRouteResult* cand_res) const {
    RerouteEvaluation e; *cand_res = base_result;
    if (candidate.target_sink_tree_node < 0 || candidate.target_sink_tree_node >= (int)cand_res->tree_nodes.size()) { e.reject_reason = "invalid_sink_node"; return e; }
    auto& sn = cand_res->tree_nodes[candidate.target_sink_tree_node];
    int ob = sn.incoming_segment_begin, oc = sn.incoming_segment_count;
    if (ob < 0 || oc < 0 || ob + oc > (int)cand_res->segments.size()) { e.reject_reason = "invalid_incoming_segment"; return e; }
    std::vector<RoutedSegment> nseg; nseg.insert(nseg.end(), cand_res->segments.begin(), cand_res->segments.begin()+ob); nseg.insert(nseg.end(), candidate.new_segments.begin(), candidate.new_segments.end()); nseg.insert(nseg.end(), cand_res->segments.begin()+ob+oc, cand_res->segments.end());
    cand_res->segments.swap(nseg); sn.parent_index = candidate.new_parent_tree_node; sn.incoming_segment_count = (int)candidate.new_segments.size();
    auto v = validator_.validateNetTopology(net, *cand_res, &hbt_manager_); e.topology_valid = v.valid; if (!v.valid) { e.reject_reason = v.reason; return e; }
    auto before = router_.evaluateTimingSummaryPublic(net, base_result); auto after = router_.evaluateTimingSummaryPublic(net, *cand_res);
    e.wirelength_before = before.total_wirelength; e.wirelength_after = after.total_wirelength; e.hbt_count_before=before.hbt_count; e.hbt_count_after=after.hbt_count;
    if (after.total_wirelength > before.total_wirelength*(1.0+params_.max_wirelength_growth_ratio)+1e-9) { e.reject_reason = "wirelength_growth"; return e; }
    e.edcompute_ready = router_.annotateDelayPublic(net,*cand_res); if (!e.edcompute_ready || !cand_res->delay_summary.ready) { e.reject_reason = "edcompute_failed"; return e; }
    e.max_delay_before = base_result.delay_summary.max_sink_delay; e.max_delay_after = cand_res->delay_summary.max_sink_delay;
    e.avg_delay_before = base_result.delay_summary.avg_sink_delay; e.avg_delay_after = cand_res->delay_summary.avg_sink_delay;
    e.objective_before = objective(params_, base_result.delay_summary, before.total_wirelength, before.hbt_count);
    e.objective_after = objective(params_, cand_res->delay_summary, after.total_wirelength, after.hbt_count);
    e.delay_improved = e.max_delay_after < e.max_delay_before || e.avg_delay_after < e.avg_delay_before;
    e.objective_improved = e.objective_after < e.objective_before;
    e.accepted = e.topology_valid && e.delay_improved && e.objective_improved;
    if (!e.accepted) e.reject_reason = "no_improvement";
    return e;
}

CriticalNetOptimizer::OptimizationStats CriticalNetOptimizer::optimize(std::vector<NetRouteResult>& results) const {
    OptimizationStats st; std::cout << "[NetReRoute] start optimization\n";
    auto complete = validator_.validateDesignCompleteness(db_.nets, results); std::cout << "[NetReRoute] design completeness check valid=" << complete.valid << " reason=" << complete.reason << "\n"; if (!complete.valid) return st;
    std::unordered_map<std::string,int> n2i; for(int i=0;i<(int)db_.nets.size();++i)n2i[db_.nets[i].name]=i;
    NetReRoute2DOptimizer d2(db_,grid_,router_,hbt_manager_,params_); NetReRoute3DOptimizer d3(db_,grid_,router_,hbt_manager_,params_);
    for (auto& result : results) {
        auto it=n2i.find(result.net_name); if (it==n2i.end() || !result.success || !result.delay_summary.ready) continue; ++st.visited_nets;
        const Net& net=db_.nets[it->second]; OptimizationStats local;
        if (!result.is_3d && params_.enable_2d_optimization) local = d2.optimize2DNet(net,result,const_cast<CriticalNetOptimizer*>(this), false);
        if (result.is_3d && params_.enable_3d_optimization) local = d3.optimize3DNet(net,result,const_cast<CriticalNetOptimizer*>(this), d2);
        st.tried_candidates += local.tried_candidates; st.accepted_candidates += local.accepted_candidates; st.rejected_candidates += local.rejected_candidates; st.improved_nets += local.improved_nets;
        auto v = validator_.validateNetTopology(net,result,&hbt_manager_); std::cout << "[Validator] net="<<net.name<<" "<<(v.valid?"valid":"invalid")<<" reason="<<v.reason<<"\n";
    }
    std::cout << "[NetReRoute] finish optimization\n"; return st;
}
