#include "NetReRoute.h"
#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>

namespace {
enum class CandidateType{kEdgeRelocation,kRipupOneSink,kHBTSwap,kHBTInsert,kHBTRemove,kCrossDieDetour};
struct Candidate{CandidateType type;int sink_pin=-1,sink_node=-1,old_parent=-1,new_parent=-1,old_hbt=-1,new_hbt=-1;double predicted_gain=0.0;bool force=false;std::string reject;std::vector<RoutedSegment> segs;};
int treeNodeForPin(const NetRouteResult&r,int pin){for(int i=0;i<(int)r.tree_nodes.size();++i)if(r.tree_nodes[i].pin_index==pin)return i;return -1;}
bool inSubtree(const NetRouteResult&r,int root,int q){std::vector<std::vector<int>>ch(r.tree_nodes.size());for(int i=0;i<(int)r.tree_nodes.size();++i){int p=r.tree_nodes[i].parent_index; if(i!=r.root_tree_index&&p>=0&&p<(int)r.tree_nodes.size()) ch[p].push_back(i);}std::queue<int>qq;qq.push(root);while(!qq.empty()){int u=qq.front();qq.pop();if(u==q)return true;for(int v:ch[u])qq.push(v);}return false;}
std::string typeName(CandidateType t){switch(t){case CandidateType::kEdgeRelocation:return"edge_relocation";case CandidateType::kRipupOneSink:return"ripup_one_sink";case CandidateType::kHBTSwap:return"hbt_swap";case CandidateType::kHBTInsert:return"hbt_insert";case CandidateType::kHBTRemove:return"hbt_remove";case CandidateType::kCrossDieDetour:return"cross_die_detour";}return"unknown";}
}
CriticalNetOptimizer::CriticalNetOptimizer(const RouterDB&d,const HybridGrid&g,const PDTreeRouter&r,HBTResourceManager&h,const Params&p):db_(d),grid_(g),router_(r),hbt_manager_(h),params_(p){}

CriticalNetOptimizer::OptimizationStats CriticalNetOptimizer::optimize(std::vector<NetRouteResult>& results) const {
    OptimizationStats st; std::unordered_map<std::string,int> n2i; for(int i=0;i<(int)db_.nets.size();++i)n2i[db_.nets[i].name]=i;
    std::vector<int> idx; for(int i=0;i<(int)results.size();++i) if(results[i].success&&results[i].delay_summary.ready) idx.push_back(i);
    std::sort(idx.begin(),idx.end(),[&](int a,int b){auto&ra=results[a];auto&rb=results[b];if(std::abs(ra.delay_summary.max_sink_delay-rb.delay_summary.max_sink_delay)>1e-12) return ra.delay_summary.max_sink_delay>rb.delay_summary.max_sink_delay;return ra.delay_summary.avg_sink_delay>rb.delay_summary.avg_sink_delay;});
    if((int)idx.size()>params_.top_k_nets) idx.resize(params_.top_k_nets);
    for(int ridx:idx){ ++st.visited_nets; auto it=n2i.find(results[ridx].net_name); if(it==n2i.end()) continue; const Net& net=db_.nets[it->second]; auto& result=results[ridx];
        int sink_pin=result.delay_summary.max_delay_pin_index; if(sink_pin<0){double md=-1;for(auto&s:result.delay_summary.sink_delays)if(s.delay>md){md=s.delay;sink_pin=s.pin_index;}}
        int sink_node=treeNodeForPin(result,sink_pin); if(sink_node<0||sink_node==result.root_tree_index) continue;
        int old_parent=result.tree_nodes[sink_node].parent_index;
        double base_max=result.delay_summary.max_sink_delay, base_avg=result.delay_summary.avg_sink_delay, base_sink=base_max;
        for(auto&s:result.delay_summary.sink_delays) if(s.pin_index==sink_pin) base_sink=s.delay;
        auto sum=router_.evaluateTimingSummaryPublic(net,result); double base_wl=sum.total_wirelength; int base_hbt=sum.hbt_count;
        auto objective=[&](const NetRouteResult&r){auto ts=router_.evaluateTimingSummaryPublic(net,r); double c=0;for(auto&s:r.delay_summary.sink_delays)if(s.pin_index==sink_pin)c=s.delay;return params_.objective_weight_max_delay*r.delay_summary.max_sink_delay+params_.objective_weight_avg_delay*r.delay_summary.avg_sink_delay+params_.objective_weight_critical_sink_delay*c+params_.objective_weight_wirelength*ts.total_wirelength+params_.objective_weight_hbt_count*ts.hbt_count;};
        double base_obj=objective(result); NetRouteResult best=result; double best_obj=base_obj; bool improved=false;
        std::vector<Candidate> cands; auto parents=router_.collectCandidateParentsPublic(net,result,net.pins[sink_pin]);
        for(int p:parents){ if(p<0||p>=(int)result.tree_nodes.size()||p==sink_node||p==old_parent||inSubtree(result,sink_node,p)) continue; Candidate c; c.sink_pin=sink_pin;c.sink_node=sink_node;c.old_parent=old_parent;c.new_parent=p; RoutedPoint sp=router_.pinToPointPublic(net.pins[sink_pin]); std::vector<RoutedSegment> seg;
            if(router_.build2DConnectionPublic(result.tree_nodes[p].point,sp,seg)){ c.type=CandidateType::kEdgeRelocation; c.segs=seg; cands.push_back(c); if(params_.enable_ripup){c.type=CandidateType::kRipupOneSink;cands.push_back(c);} if(params_.enable_hbt_insert){Candidate hi=c;hi.type=CandidateType::kHBTInsert;cands.push_back(hi);}  }
            if(params_.enable_cross_die_detour){ auto free=hbt_manager_.collectFreeHBTsNear(grid_, Point2D{sp.x,sp.y}, params_.max_hbt_candidates_per_branch); if(!free.empty()){std::vector<RoutedSegment>s2; RoutedPoint hp=sp; hp.x=result.tree_nodes[p].point.x; hp.y=result.tree_nodes[p].point.y; RoutedSegment a; a.p1=result.tree_nodes[p].point; a.p2=hp; a.uses_hbt=true; a.hbt_id=free.front();
            RoutedSegment b; b.p1=hp; b.p2=sp; b.uses_hbt=false; b.hbt_id=-1; s2={a,b}; Candidate d; d.type=CandidateType::kCrossDieDetour;d.sink_pin=sink_pin;d.sink_node=sink_node;d.old_parent=old_parent;d.new_parent=p;d.new_hbt=free.front();d.segs=s2;d.force=params_.debug_force_accept_cross_die_detour; cands.push_back(d);} else st.rejected_by_no_free_hbt++; }
        }
        for(auto &c:cands){ ++st.tried_candidates; if(c.type==CandidateType::kEdgeRelocation)st.tried_edge_relocation++; if(c.type==CandidateType::kRipupOneSink)st.tried_ripup++; if(c.type==CandidateType::kCrossDieDetour)st.tried_cross_die_detour++;
            NetRouteResult w=result; auto &sn=w.tree_nodes[c.sink_node]; int ob=sn.incoming_segment_begin, oc=sn.incoming_segment_count; if(ob<0||oc<0||ob+oc>(int)w.segments.size()){st.rejected_by_topology++;continue;}
            std::vector<RoutedSegment> nseg; nseg.insert(nseg.end(),w.segments.begin(),w.segments.begin()+ob); nseg.insert(nseg.end(),c.segs.begin(),c.segs.end()); nseg.insert(nseg.end(),w.segments.begin()+ob+oc,w.segments.end()); w.segments.swap(nseg); sn.parent_index=c.new_parent; sn.incoming_segment_count=(int)c.segs.size(); sn.incoming_hbt_count=0; sn.incoming_hbt_res=sn.incoming_hbt_cap=0; sn.assigned_hbt_id=-1; for(auto&s:c.segs){if(s.uses_hbt){sn.incoming_hbt_count++;sn.assigned_hbt_id=s.hbt_id;sn.incoming_hbt_res+=db_.hbt.parasitic_res;sn.incoming_hbt_cap+=db_.hbt.parasitic_cap;}}
            if(!router_.annotateDelayPublic(net,w) || !w.delay_summary.ready){st.rejected_by_topology++;continue;}
            auto ws=router_.evaluateTimingSummaryPublic(net,w);
            if(ws.total_wirelength>base_wl*(1.0+params_.max_wirelength_growth_ratio)+1e-9){st.rejected_by_wirelength++;continue;}
            if(ws.hbt_count>base_hbt+params_.max_extra_hbts){st.rejected_by_wirelength++;continue;}
            double sink_after=w.delay_summary.max_sink_delay; for(auto&s:w.delay_summary.sink_delays) if(s.pin_index==sink_pin)sink_after=s.delay;
            bool timing=(sink_after<base_sink)||(w.delay_summary.max_sink_delay<base_max)||(w.delay_summary.avg_sink_delay<base_avg);
            double obj=objective(w);
            if((timing&&obj<best_obj) || c.force){best=w;best_obj=obj;improved=true;++st.accepted_candidates; if(c.type==CandidateType::kEdgeRelocation)st.accepted_edge_relocation++; if(c.type==CandidateType::kRipupOneSink)st.accepted_ripup++; if(c.type==CandidateType::kCrossDieDetour)st.accepted_cross_die_detour++; }
            else st.rejected_by_no_delay_gain++;
        }
        if(improved){result=best; st.improved_nets++; if(params_.verbose) std::cout<<"[reroute] net="<<net.name<<" improved by candidate flow\n";}
    }
    return st;
}
