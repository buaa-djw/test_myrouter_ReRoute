#pragma once
#include "ExperimentConfig.h"
#include "PDTreeRouter.h"
#include "RouterDB.h"
#include <string>
struct CriticalNetOptimizerStatsProxy { int visited_nets=0, improved_nets=0, tried_candidates=0, accepted_candidates=0, rejected_by_topology=0, rejected_by_delay=0, rejected_by_wirelength=0, rejected_by_hbt_conflict=0; int tried_reattach_candidates=0,tried_ripup_candidates=0,accepted_reattach_candidates=0,accepted_ripup_candidates=0,rejected_by_cycle=0,rejected_by_cross_die_not_supported=0,rejected_by_invalid_hbt=0; int hbt_conflict_before=0,hbt_conflict_after=0; double total_objective_before=0.0,total_objective_after=0.0,total_wirelength_before=0.0,total_wirelength_after=0.0; int total_hbt_count_before=0,total_hbt_count_after=0; };
void writeExperimentSummary(const std::string& path,const ExperimentConfig& cfg,const RouterDB& db,const std::vector<NetRouteResult>& results,const std::string& start_ts,const std::string& end_ts,double runtime_sec,const CriticalNetOptimizerStatsProxy* reroute_stats=nullptr);
