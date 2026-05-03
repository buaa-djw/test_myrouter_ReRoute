#pragma once

#include "RouterDB.h"

#include <optional>
#include <string>

struct DefaultLayerRC
{
    double resistance = 0.0;
    double capacitance = 0.0;
};

struct DefaultWireRC
{
    double resistance = 0.0;
    double capacitance = 0.0;
};

class RCDefaults
{
public:
    // Look up fallback layer RC by layer name.
    // Name matching is case-insensitive and exact after normalization.
    static std::optional<DefaultLayerRC> findLayerRC(const std::string& layer_name);

    // Look up fallback via resistance by via name.
    // Name matching is case-insensitive and exact after normalization.
    static std::optional<double> findViaResistance(const std::string& via_name);

    // Fallback wire RC by net class.
    static DefaultWireRC signalWireRC();
    static DefaultWireRC clockWireRC();
    static DefaultWireRC wireRCForNetClass(NetClass net_class);
};

