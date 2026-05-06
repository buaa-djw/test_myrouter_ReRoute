#pragma once
#include "HBTResourceManager.h"
#include "RouterDB.h"
#include <string>
#include <vector>

class RouteTopologyValidator {
public:
    struct Result {
        bool valid = true;
        bool has_cycle = false;
        bool has_floating_segment = false;
        bool has_unreachable_sink = false;
        bool has_invalid_index = false;
        bool has_invalid_hbt_connection = false;
        bool has_missing_net_result = false;
        int reachable_sink_count = 0;
        int expected_sink_count = 0;
        int routed_net_count = 0;
        int expected_net_count = 0;
        std::string reason;
    };

    Result validateNetTopology(const Net& net, const NetRouteResult& result, const HBTResourceManager* hbt_manager) const;
    Result validateDesignCompleteness(const std::vector<Net>& nets, const std::vector<NetRouteResult>& results) const;
};
