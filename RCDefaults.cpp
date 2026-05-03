#include "RCDefaults.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace {

std::string toUpper(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

const std::unordered_map<std::string, DefaultLayerRC>& kDefaultLayerRCMap()
{
    static const std::unordered_map<std::string, DefaultLayerRC> kMap = {
        {"M1", {5.4286e-03, 7.41819E-02}},
        {"M2", {3.5714e-03, 6.74606E-02}},
        {"M3", {3.5714e-03, 8.88758E-02}},
        {"M4", {1.5000e-03, 1.07121E-01}},
        {"M5", {1.5000e-03, 1.08964E-01}},
        {"M6", {1.5000e-03, 1.02044E-01}},
        {"M7", {1.8750e-04, 1.10436E-01}},
        {"M8", {1.8750e-04, 9.69714E-02}},
        {"M9", {3.7500e-05, 3.6864e-02}},
        {"M10", {3.7500e-05, 2.8042e-02}},
        {"M2_ADD", {4.62311E-02, 1.84542E-01}},
        {"M3_ADD", {3.63251E-02, 1.53955E-01}},
        {"M1_M", {7.04175E-02, 1e-10}},
        {"M2_M", {4.62311E-02, 1.84542E-01}},
        {"M3_M", {3.63251E-02, 1.53955E-01}},
        {"M4_M", {2.03083E-02, 1.89434E-01}},
        {"M5_M", {1.93005E-02, 1.71593E-01}},
        {"M6_M", {1.18619E-02, 1.76146E-01}},
    };
    return kMap;
}

const std::unordered_map<std::string, double>& kDefaultViaRMap()
{
    static const std::unordered_map<std::string, double> kMap = {
        {"V1_ADD", 1.72E-02},
        {"V2_ADD", 1.72E-02},
        {"V1_M", 1.72E-02},
        {"V2_M", 1.72E-02},
        {"V3_M", 1.72E-02},
        {"V4_M", 1.18E-02},
        {"V5_M", 1.18E-02},
        {"HB_LAYER", 6.30E-03},
    };
    return kMap;
}

constexpr DefaultWireRC kSignalWireRC{3.23151E-02, 1.73323E-01};
constexpr DefaultWireRC kClockWireRC{5.13971E-02, 1.44549E-01};

}  // namespace

std::optional<DefaultLayerRC> RCDefaults::findLayerRC(const std::string& layer_name)
{
    const auto& m = kDefaultLayerRCMap();
    const auto it = m.find(toUpper(layer_name));
    if (it == m.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<double> RCDefaults::findViaResistance(const std::string& via_name)
{
    const auto& m = kDefaultViaRMap();
    const auto it = m.find(toUpper(via_name));
    if (it == m.end()) {
        return std::nullopt;
    }
    return it->second;
}

DefaultWireRC RCDefaults::signalWireRC()
{
    return kSignalWireRC;
}

DefaultWireRC RCDefaults::clockWireRC()
{
    return kClockWireRC;
}

DefaultWireRC RCDefaults::wireRCForNetClass(NetClass net_class)
{
    if (net_class == NetClass::kClock) {
        return clockWireRC();
    }
    return signalWireRC();
}

