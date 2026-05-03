#pragma once
#include "PDTreeRouter.h"
#include "RouterDB.h"
#include <string>
void writeNetInfo(const std::string& path,const RouterDB& db,const std::vector<NetRouteResult>& results);
