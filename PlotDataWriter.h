#pragma once

#include "PDTreeRouter.h"

#include <string>

std::string safeNetName(const std::string& name);
void writeNetPlotData(const std::string& root_dir, const NetRouteResult& result);
void write3DNetPlotData(const std::string& dir, const NetRouteResult& result);
