#pragma once
#include "ExperimentConfig.h"
#include "PDTreeRouter.h"
#include "RouterDB.h"
#include <string>
void writeExperimentSummary(const std::string& path,const ExperimentConfig& cfg,const RouterDB& db,const std::vector<NetRouteResult>& results,const std::string& start_ts,const std::string& end_ts,double runtime_sec);
