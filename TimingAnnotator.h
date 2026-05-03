#pragma once

#include "LibertyData.h"
#include "RouterDB.h"
#include "VerilogNetlist.h"

struct TimingAnnotationSummary
{
    int nets_with_driver = 0;
    int nets_without_driver = 0;
    int pins_with_input_cap = 0;
    int pins_without_input_cap = 0;
};

class TimingAnnotator
{
public:
    TimingAnnotationSummary annotate(RouterDB& db,
                                     const VerilogNetlist& vnet,
                                     const LibertyData& upper_lib,
                                     const LibertyData& bottom_lib) const;
};
