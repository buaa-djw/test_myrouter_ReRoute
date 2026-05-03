#pragma once

#include "PDTreeRouter.h"
#include "RouterDB.h"

class EDCompute
{
public:
    struct Params {
        double default_sink_cap = 0.001;
        double default_driver_res = 0.0;

        // Fallback HBT RC if DB lacks dedicated HBT parasitics.
        double default_hbt_res = 0.0;
        double default_hbt_cap = 0.0;
        // When true, ignore per-node/tree-stored HBT RC and always use
        // default_hbt_res/default_hbt_cap (scaled by incoming_hbt_count).
        bool override_tree_hbt_rc = false;

        bool verbose = false;
    };

    explicit EDCompute(const RouterDB& db);
    EDCompute(const RouterDB& db, const Params& params);

    // Annotate per-net delay summary from routed tree structure.
    // Returns true on successful annotation (delay_summary.ready=true).
    bool annotateNetDelay(const Net& net, NetRouteResult& result) const;

private:
    const RouterDB& db_;
    Params params_;
};
