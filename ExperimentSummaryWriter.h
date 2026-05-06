#pragma once
#include "ExperimentConfig.h"
#include "PDTreeRouter.h"
#include "RouterDB.h"
#include <string>
struct CriticalNetOptimizerStatsProxy { int visited_nets=0; int improved_nets=0; int tried_candidates=0; int accepted_candidates=0; int rejected_by_topology=0; int rejected_by_delay=0; int rejected_by_wirelength=0; int rejected_by_hbt_conflict=0; int hbt_conflict_before=0; int hbt_conflict_after=0; double total_objective_before=0.0; double total_objective_after=0.0; int tried_cross_die_ripup_candidates=0; int accepted_cross_die_ripup_candidates=0; int rejected_by_no_free_hbt=0; int rejected_by_cross_die_not_supported=0; int rejected_by_build_hbt_branch_failed=0; int force_accepted_cross_die_ripup_candidates=0; };
void writeExperimentSummary(const std::string& path,const ExperimentConfig& cfg,const RouterDB& db,const std::vector<NetRouteResult>& results,const std::string& start_ts,const std::string& end_ts,double runtime_sec,const CriticalNetOptimizerStatsProxy* reroute_stats=nullptr);
