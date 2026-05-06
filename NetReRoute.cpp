#include "NetReRoute.h"
#include "NetReRoute2D.h"
#include "NetReRoute3D.h"
#include <algorithm>
#include <fstream>
#include <unordered_map>

namespace {
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

bool CriticalNetOptimizer::evaluateAndMaybeApplyCandidateForModule(const Net& net, NetRouteResult& result, const RerouteCandidate& candidate, OptimizationStats* stats, bool allow_force_accept, std::string* reject_reason) const {
    NetRouteResult cand; auto ev = evaluateCandidate(net, result, candidate, &cand);
    if (stats) { ++stats->tried_candidates; if (ev.accepted) ++stats->accepted_candidates; else ++stats->rejected_candidates; }
    if (ev.accepted || (allow_force_accept && params_.debug_force_accept)) { result = cand; if (reject_reason) reject_reason->clear(); return true; }
    if (reject_reason) *reject_reason = ev.reject_reason;
    return false;
}


bool CriticalNetOptimizer::trySameDieReattachForModule(const Net& net, NetRouteResult& result, int sink_tree_node, int sink_pin_index, int new_parent_tree_node, bool fixed_hbt, OptimizationStats* stats, std::string* reject_reason) const {
    if (sink_tree_node < 0 || sink_tree_node >= (int)result.tree_nodes.size() || new_parent_tree_node < 0 || new_parent_tree_node >= (int)result.tree_nodes.size()) return false;
    const auto& parent = result.tree_nodes[new_parent_tree_node];
    const auto& sink = result.tree_nodes[sink_tree_node];
    RerouteCandidate c; c.net_name=net.name; c.is_3d=result.is_3d; c.candidate_type=fixed_hbt?RerouteCandidateType::k3DIntraDieReattachWithFixedHBT:RerouteCandidateType::k2DReattach; c.target_sink_pin_index=sink_pin_index; c.target_sink_tree_node=sink_tree_node; c.new_parent_tree_node=new_parent_tree_node;
    RoutedSegment seg; seg.p1=parent.point; seg.p2=sink.point; seg.uses_hbt=false; seg.hbt_id=-1; c.new_segments={seg};
    return evaluateAndMaybeApplyCandidateForModule(net,result,c,stats,false,reject_reason);
}
CriticalNetOptimizer::OptimizationStats CriticalNetOptimizer::runCrossDieDetourForModule(const Net& net, NetRouteResult& result) const {
    OptimizationStats st; if (!result.is_3d) return st;
    int sink_node=-1; int sink_pin=result.delay_summary.max_delay_pin_index;
    for(int i=0;i<(int)result.tree_nodes.size();++i) if(result.tree_nodes[i].pin_index==sink_pin){sink_node=i;break;}
    if (sink_node<=0) return st;
    const auto& sink = result.tree_nodes[sink_node];
    for(int p=0;p<(int)result.tree_nodes.size();++p){
        if(p==sink_node) continue; const auto& parent=result.tree_nodes[p]; if(parent.point.die==sink.point.die) continue;
        Point2D qp{sink.point.x,sink.point.y}; auto free_hbts=hbt_manager_.collectFreeHBTsNear(grid_, qp, 4);
        for(int hid: free_hbts){
            const auto& hpt = grid_.hbt.getSlot(hid);
            RoutedSegment s1{{parent.point.x,parent.point.y,parent.point.die},{hpt.x,hpt.y,parent.point.die},-1,true,hid};
            RoutedSegment s2{{hpt.x,hpt.y,sink.point.die},{sink.point.x,sink.point.y,sink.point.die},-1,true,hid};
            RerouteCandidate c; c.net_name=net.name; c.is_3d=true; c.candidate_type=RerouteCandidateType::k3DCrossLayerDetour; c.target_sink_pin_index=sink_pin; c.target_sink_tree_node=sink_node; c.new_parent_tree_node=p; c.new_hbt_id=hid; c.new_segments={s1,s2};
            ++st.tried_cross_die_ripup_candidates;
            if (evaluateAndMaybeApplyCandidateForModule(net,result,c,&st,false,nullptr)) { ++st.accepted_cross_die_ripup_candidates; st.improved_nets=1; return st; }
        }
    }
    return st;
}

CriticalNetOptimizer::OptimizationStats CriticalNetOptimizer::runHBTSwapForModule(const Net& net, NetRouteResult& result) const {
    OptimizationStats st; 
    for (auto& seg : result.segments) {
        if (!seg.uses_hbt || seg.hbt_id < 0) continue;
        Point2D qp{seg.p1.x,seg.p1.y};
        auto free_hbts = hbt_manager_.collectFreeHBTsNear(grid_, qp, 4);
        for (int hid : free_hbts) {
            NetRouteResult work = result;
            for (auto& s : work.segments) if (s.uses_hbt && s.hbt_id==seg.hbt_id) s.hbt_id=hid;
            auto v = validator_.validateNetTopology(net, work, &hbt_manager_);
            ++st.tried_candidates;
            if (!v.valid || !router_.annotateDelayPublic(net, work)) { ++st.rejected_candidates; continue; }
            if (work.delay_summary.max_sink_delay + 1e-12 < result.delay_summary.max_sink_delay) { result=work; ++st.accepted_candidates; st.improved_nets=1; return st; }
            ++st.rejected_candidates;
        }
    }
    return st;
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
