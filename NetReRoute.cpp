#include "NetReRoute.h"
#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>

namespace {
int treeNodeForPin(const NetRouteResult&r,int pin){for(int i=0;i<(int)r.tree_nodes.size();++i)if(r.tree_nodes[i].pin_index==pin)return i;return -1;}
bool inSubtree(const NetRouteResult&r,int root,int q){std::vector<std::vector<int>>ch(r.tree_nodes.size());for(int i=0;i<(int)r.tree_nodes.size();++i){int p=r.tree_nodes[i].parent_index; if(i!=r.root_tree_index&&p>=0&&p<(int)r.tree_nodes.size()) ch[p].push_back(i);}std::queue<int>qq;qq.push(root);while(!qq.empty()){int u=qq.front();qq.pop();if(u==q)return true;for(int v:ch[u])qq.push(v);}return false;}
int mdist(const RoutedPoint&a,const RoutedPoint&b){return std::abs(a.x-b.x)+std::abs(a.y-b.y);} 
std::string editTypeToString(CriticalNetOptimizer::EditType t){ if(t==CriticalNetOptimizer::EditType::kCrossDieRipupViaHBT) return "cross_die_detour_via_hbt"; return "other"; }
}
CriticalNetOptimizer::CriticalNetOptimizer(const RouterDB&d,const HybridGrid&g,const PDTreeRouter&r,HBTResourceManager&h,const Params&p):db_(d),grid_(g),router_(r),hbt_manager_(h),params_(p){}

std::vector<CriticalNetOptimizer::NetEditCandidate> CriticalNetOptimizer::generateCrossDieDetourCandidates(const Net& net, const NetRouteResult& result, int sink_pin_index, int sink_tree_node, OptimizationStats* stats) const{
    std::vector<NetEditCandidate> candidates;
    if (sink_tree_node < 0 || sink_tree_node >= (int)result.tree_nodes.size()) return candidates;
    const RoutedPoint sink_point = result.tree_nodes[sink_tree_node].point;
    if (!result.is_3d) { if (stats) stats->rejected_by_cross_die_not_supported++; return candidates; }
    std::vector<std::pair<int,int>> parents;
    for(int p=0;p<(int)result.tree_nodes.size();++p){
        if(p==sink_tree_node || inSubtree(result,sink_tree_node,p)) continue;
        const auto& pp=result.tree_nodes[p].point;
        if(pp.die==sink_point.die) continue;
        parents.push_back({mdist(pp,sink_point),p});
    }
    if(parents.empty()){ if (stats) stats->rejected_by_cross_die_not_supported++; return candidates; }
    std::sort(parents.begin(),parents.end());
    int parent_limit=std::max(1,std::min(params_.beam_width, params_.max_iterations_per_net));
    int old_parent = result.tree_nodes[sink_tree_node].parent_index;
    for(int i=0;i<(int)parents.size()&&i<parent_limit;++i){
        int p=parents[i].second;
        const RoutedPoint parent_point=result.tree_nodes[p].point;
        auto free=hbt_manager_.collectFreeHBTsNear(grid_, Point2D{sink_point.x,sink_point.y}, params_.max_hbt_candidates_per_branch);
        if(free.empty()){ if (stats) stats->rejected_by_no_free_hbt++; continue; }
        int hlimit=std::min((int)free.size(), params_.max_hbt_candidates_per_branch);
        for(int k=0;k<hlimit;++k){
            RoutedPoint hp = sink_point; hp.x=parent_point.x; hp.y=parent_point.y;
            RoutedSegment a; a.p1=parent_point; a.p2=hp; a.uses_hbt=true; a.hbt_id=free[k];
            RoutedSegment b; b.p1=hp; b.p2=sink_point; b.uses_hbt=false; b.hbt_id=-1;
            if (a.p1.die == a.p2.die) { if (stats) stats->rejected_by_build_hbt_branch_failed++; continue; }
            NetEditCandidate cand; cand.type=EditType::kCrossDieRipupViaHBT; cand.sink_pin_index=sink_pin_index; cand.sink_tree_node=sink_tree_node; cand.old_parent_tree_node=old_parent; cand.new_parent_tree_node=p; cand.new_hbt_id=free[k]; cand.new_hbt_ids={free[k]}; cand.inserted_hbt_ids={free[k]}; cand.new_segments={a,b}; cand.has_child_attach_point=true; cand.child_attach_point=sink_point; cand.reject_reason.clear();
            candidates.push_back(cand);
            if (stats) stats->tried_cross_die_ripup_candidates++;
            if ((int)candidates.size() >= params_.max_iterations_per_net) return candidates;
        }
    }
    return candidates;
}

CriticalNetOptimizer::OptimizationStats CriticalNetOptimizer::optimize(std::vector<NetRouteResult>& results) const {
    OptimizationStats st; std::unordered_map<std::string,int> n2i; for(int i=0;i<(int)db_.nets.size();++i)n2i[db_.nets[i].name]=i;
    std::vector<int> idx; for(int i=0;i<(int)results.size();++i) if(results[i].success&&results[i].delay_summary.ready) idx.push_back(i);
    std::sort(idx.begin(),idx.end(),[&](int a,int b){auto&ra=results[a];auto&rb=results[b];if(std::abs(ra.delay_summary.max_sink_delay-rb.delay_summary.max_sink_delay)>1e-12) return ra.delay_summary.max_sink_delay>rb.delay_summary.max_sink_delay;return ra.delay_summary.avg_sink_delay>rb.delay_summary.avg_sink_delay;});
    if((int)idx.size()>params_.top_k_nets) idx.resize(params_.top_k_nets);
    for(int ridx:idx){ ++st.visited_nets; auto it=n2i.find(results[ridx].net_name); if(it==n2i.end()) continue; const Net& net=db_.nets[it->second]; auto& result=results[ridx];
        int sink_pin=result.delay_summary.max_delay_pin_index; if(sink_pin<0){double md=-1;for(auto&s:result.delay_summary.sink_delays)if(s.delay>md){md=s.delay;sink_pin=s.pin_index;}}
        int sink_node=treeNodeForPin(result,sink_pin); if(sink_node<0||sink_node==result.root_tree_index) continue;
        double base_max=result.delay_summary.max_sink_delay, base_avg=result.delay_summary.avg_sink_delay, base_sink=base_max;
        for(auto&s:result.delay_summary.sink_delays) if(s.pin_index==sink_pin) base_sink=s.delay;
        auto sum=router_.evaluateTimingSummaryPublic(net,result); double base_wl=sum.total_wirelength; int base_hbt=sum.hbt_count;
        NetRouteResult best=result; bool improved=false;
        std::vector<NetEditCandidate> cands;
        if(params_.enable_cross_die_ripup || params_.enable_cross_die_detour){
            auto gen = generateCrossDieDetourCandidates(net, result, sink_pin, sink_node, &st);
            cands.insert(cands.end(), gen.begin(), gen.end());
            if (gen.empty() && params_.verbose) std::cout << "[reroute] no cross-die detour candidates for net=" << net.name << "\n";
        }
        for(auto &c:cands){ ++st.tried_candidates;
            NetRouteResult w=result; auto &sn=w.tree_nodes[c.sink_tree_node]; int ob=sn.incoming_segment_begin, oc=sn.incoming_segment_count; if(ob<0||oc<0||ob+oc>(int)w.segments.size()){st.rejected_by_topology++;continue;}
            std::vector<RoutedSegment> nseg; nseg.insert(nseg.end(),w.segments.begin(),w.segments.begin()+ob); nseg.insert(nseg.end(),c.new_segments.begin(),c.new_segments.end()); nseg.insert(nseg.end(),w.segments.begin()+ob+oc,w.segments.end()); w.segments.swap(nseg); sn.parent_index=c.new_parent_tree_node; sn.incoming_segment_count=(int)c.new_segments.size(); sn.incoming_hbt_count=0; sn.incoming_hbt_res=sn.incoming_hbt_cap=0; sn.assigned_hbt_id=-1; for(auto&s:c.new_segments){if(s.uses_hbt){ if(hbt_manager_.isOccupied(s.hbt_id)){st.rejected_by_hbt_conflict++; goto NEXTC;} sn.incoming_hbt_count++;sn.assigned_hbt_id=s.hbt_id;sn.incoming_hbt_res+=db_.hbt.parasitic_res;sn.incoming_hbt_cap+=db_.hbt.parasitic_cap;}}
            if(!router_.annotateDelayPublic(net,w) || !w.delay_summary.ready){st.rejected_by_topology++;continue;}
            {auto ws=router_.evaluateTimingSummaryPublic(net,w);
            if(ws.total_wirelength>base_wl*(1.0+params_.max_wirelength_growth_ratio)+1e-9){st.rejected_by_wirelength++;continue;}
            if(ws.hbt_count>base_hbt+params_.max_extra_hbts){st.rejected_by_wirelength++;continue;}
            double sink_after=w.delay_summary.max_sink_delay; for(auto&s:w.delay_summary.sink_delays) if(s.pin_index==sink_pin)sink_after=s.delay;
            bool timing=(sink_after<base_sink)||(w.delay_summary.max_sink_delay<base_max)||(w.delay_summary.avg_sink_delay<base_avg);
            bool force_ok=(c.type==EditType::kCrossDieRipupViaHBT)&&(params_.debug_force_accept_cross_die_ripup||params_.debug_force_accept_cross_die_detour);
            if(timing || force_ok){best=w;improved=true;++st.accepted_candidates; if(c.type==EditType::kCrossDieRipupViaHBT){st.accepted_cross_die_ripup_candidates++; if(force_ok){st.force_accepted_cross_die_ripup_candidates++; if(params_.verbose) std::cout<<"FORCE_ACCEPT "<<editTypeToString(c.type)<<" net="<<net.name<<"\n";} hbt_manager_.reserve(c.new_hbt_id, net.name, -1, c.sink_tree_node, c.sink_pin_index);} break;} else st.rejected_by_delay++; }
            NEXTC: ;
        }
        if(improved){result=best; st.improved_nets++; if(params_.verbose) std::cout<<"[reroute] net="<<net.name<<" improved by candidate flow\n";}
    }
    return st;
}
