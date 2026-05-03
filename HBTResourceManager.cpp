#include "HBTResourceManager.h"
#include <algorithm>

void HBTResourceManager::initializeFromGrid(const HybridGrid& grid){ clear(); reservations_.resize(grid.hbtSlotCount()); for(int i=0;i<(int)reservations_.size();++i){reservations_[i].hbt_id=i;} }
void HBTResourceManager::clear(){ reservations_.clear(); stats_=Stats{}; }
bool HBTResourceManager::reserve(int hbt_id,const std::string& n,int ri,int tn,int pi){ if(hbt_id<0||hbt_id>=(int)reservations_.size()){++stats_.invalid_hbt_id_count;return false;} auto& r=reservations_[hbt_id]; if(r.occupied && r.owner_net!=n){++stats_.conflict_count; return false;} r.occupied=true; r.owner_net=n; r.owner_result_index=ri; r.owner_tree_node=tn; r.owner_pin_index=pi; return true; }
bool HBTResourceManager::release(int hbt_id,const std::string& n){ if(hbt_id<0||hbt_id>=(int)reservations_.size()) return false; auto& r=reservations_[hbt_id]; if(!r.occupied||r.owner_net!=n) return false; r=HBTReservation{}; r.hbt_id=hbt_id; return true; }
bool HBTResourceManager::isFree(int hbt_id) const { return hbt_id>=0&&hbt_id<(int)reservations_.size()&&!reservations_[hbt_id].occupied; }
bool HBTResourceManager::isOccupied(int hbt_id) const { return hbt_id>=0&&hbt_id<(int)reservations_.size()&&reservations_[hbt_id].occupied; }
bool HBTResourceManager::isOwnedBy(int hbt_id,const std::string& n) const { return hbt_id>=0&&hbt_id<(int)reservations_.size()&&reservations_[hbt_id].occupied&&reservations_[hbt_id].owner_net==n; }
const HBTReservation* HBTResourceManager::getReservation(int hbt_id) const { if(hbt_id<0||hbt_id>=(int)reservations_.size()) return nullptr; return &reservations_[hbt_id]; }
HBTResourceManager::Snapshot HBTResourceManager::makeSnapshot() const { return Snapshot{reservations_}; }
void HBTResourceManager::rollback(const Snapshot& s){ reservations_=s.reservations; }
HBTResourceManager::Stats HBTResourceManager::collectStats() const { Stats st=stats_; st.total_slots=reservations_.size(); st.occupied_slots=0; for(auto& r:reservations_) if(r.occupied) ++st.occupied_slots; st.free_slots=st.total_slots-st.occupied_slots; return st; }
bool HBTResourceManager::validateNoConflict(std::ostream* os) const { auto st=collectStats(); if(os) *os<<"[HBTResourceManager] total="<<st.total_slots<<" occ="<<st.occupied_slots<<" free="<<st.free_slots<<" conflict="<<st.conflict_count<<" invalid_hbt_id="<<st.invalid_hbt_id_count<<"\n"; return st.conflict_count==0; }
bool HBTResourceManager::rebuildFromRouteResults(const std::vector<NetRouteResult>& results,std::ostream* os){ for(auto& r:reservations_){auto id=r.hbt_id; r=HBTReservation{}; r.hbt_id=id;} stats_.conflict_count=0; for(int i=0;i<(int)results.size();++i){const auto& rr=results[i]; for(const auto& tn:rr.tree_nodes){ if(tn.assigned_hbt_id>=0 && !reserve(tn.assigned_hbt_id,rr.net_name,i,tn.tree_index,tn.pin_index) && os) *os<<"[HBTResourceManager] warning conflict at hbt="<<tn.assigned_hbt_id<<" net="<<rr.net_name<<"\n"; }} return stats_.conflict_count==0; }
std::vector<int> HBTResourceManager::collectFreeHBTsNear(const HybridGrid& grid,const Point2D& p,int max_count) const { std::vector<int> q=grid.hbt.queryNearestSlots(p.x,p.y,std::max(1,max_count*4)); std::vector<int> out; for(int h:q){ if(isFree(h)){ out.push_back(h); if((int)out.size()>=max_count) break; } } return out; }
