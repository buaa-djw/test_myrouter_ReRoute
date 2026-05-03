#include "RouterDB.h"
#include "RCDefaults.h"
#include "odb/geom.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::string toUpper(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

std::string trim(const std::string& s)
{
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])) != 0) {
        ++b;
    }
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) {
        --e;
    }
    return s.substr(b, e - b);
}

std::string normalizeLefLine(const std::string& line)
{
    std::string s = line;
    for (char& c : s) {
        if (c == ';' || c == '(' || c == ')' || c == ',') {
            c = ' ';
        }
    }
    return trim(s);
}

std::vector<std::string> splitTokens(const std::string& s)
{
    std::stringstream ss(s);
    std::vector<std::string> out;
    std::string tok;
    while (ss >> tok) {
        out.push_back(tok);
    }
    return out;
}

bool parseDouble(const std::string& s, double& out)
{
    try {
        std::size_t idx = 0;
        out = std::stod(s, &idx);
        return idx == s.size();
    } catch (...) {
        return false;
    }
}

bool isValidPositive(double v)
{
    return v > 0.0;
}

bool containsInsensitive(const std::string& s, const std::string& key)
{
    return toUpper(s).find(toUpper(key)) != std::string::npos;
}

bool isTopName(const std::string& s)
{
    return containsInsensitive(s, "TOP")
        || containsInsensitive(s, "UPPER")
        || containsInsensitive(s, "__UPPER")
        || containsInsensitive(s, "_UPPER");
}

bool isBottomName(const std::string& s)
{
    return containsInsensitive(s, "BOTTOM")
        || containsInsensitive(s, "__BOTTOM")
        || containsInsensitive(s, "_BOTTOM");
}

const char* dieToString(DieId die)
{
    switch (die) {
        case DieId::kTop: return "Top";
        case DieId::kBottom: return "Bottom";
        default: return "Unknown";
    }
}

int micronToDBU(double v_micron, int dbu_per_micron)
{
    return static_cast<int>(std::llround(v_micron * static_cast<double>(std::max(1, dbu_per_micron))));
}


bool looksLikeClockNetName(const std::string& net_name)
{
    const std::string u = toUpper(net_name);
    if (u.find("RESET") != std::string::npos || u.find("RST") != std::string::npos) {
        return false;
    }
    return (u.find("CLOCK") != std::string::npos || u.find("CLK") != std::string::npos);
}

bool hasClockSigTypeFromOpenDB(const odb::dbNet* /*db_net*/)
{
    // NOTE:
    //   Some OpenDB versions expose signal-type APIs differently. Keep this
    //   conservative fallback path stable in this prototype branch.
    return false;
}

NetClass classifyNetFromSignalType(const odb::dbNet* db_net, const std::string& net_name, bool is_special)
{
    if (is_special) {
        return NetClass::kSpecial;
    }

    if (hasClockSigTypeFromOpenDB(db_net)) {
        return NetClass::kClock;
    }

    if (looksLikeClockNetName(net_name)) {
        return NetClass::kClock;
    }

    return NetClass::kNormal;
}

void emitRouterDBWarning(const std::string& msg)
{
    static int warn_count = 0;
    constexpr int kWarnLimit = 20;
    if (warn_count < kWarnLimit) {
        std::cerr << "[RouterDB] warning: " << msg << "\n";
    } else if (warn_count == kWarnLimit) {
        std::cerr << "[RouterDB] warning: suppressing additional warnings after "
                  << kWarnLimit << " messages\n";
    }
    ++warn_count;
}

}  // namespace


const char* netClassToString(NetClass c)
{
    switch (c) {
        case NetClass::kNormal: return "normal";
        case NetClass::kUpperOnly: return "upper_only";
        case NetClass::kBottomOnly: return "bottom_only";
        case NetClass::kMixed: return "mixed";
        case NetClass::kClock: return "clock";
        case NetClass::kSpecial: return "special";
        case NetClass::kUnknown: return "unknown";
    }
    return "unknown";
}

void RouterDB::clear()
{
    die_lx = die_ly = die_ux = die_uy = 0;
    dbu_per_micron = 0;

    routing_layers.clear();
    hbt = {};

    nodes.clear();
    nets.clear();
    node_name_to_id.clear();
    net_name_to_id_.clear();
    via_resistance_from_db_.clear();
    has_hb_layer_resistance_from_db_ = false;
    hb_layer_resistance_from_db_ = 0.0;
}

void RouterDB::build(odb::dbDatabase* db,
                     odb::dbBlock* block,
                     const std::vector<std::string>& lef_files)
{
    clear();

    if (db == nullptr || block == nullptr) {
        std::cerr << "[RouterDB::build] db or block is null." << std::endl;
        return;
    }

    if (block->getDbUnitsPerMicron() > 0) {
        dbu_per_micron = block->getDbUnitsPerMicron();
    } else if (db->getTech() != nullptr) {
        dbu_per_micron = db->getTech()->getDbUnitsPerMicron();
    }

    buildDieArea(block);
    buildRoutingLayers(db->getTech(), block, lef_files);
    buildNodes(block);
    buildNets(block);
    extractHBTInfo(db, lef_files);

    std::cout << "[RouterDB::build] done\n"
              << "  dbu_per_micron = " << dbu_per_micron << "\n"
              << "  nodes          = " << nodes.size() << "\n"
              << "  nets           = " << nets.size() << "\n"
              << "  2D nets        = " << get2DNetCount() << "\n"
              << "  3D nets        = " << get3DNetCount() << "\n"
              << "  routing layers = " << routing_layers.size() << "\n"
              << "  HBT found      = " << (hbt.found ? "yes" : "no") << "\n";
}

void RouterDB::buildDieArea(odb::dbBlock* block)
{
    if (block == nullptr) {
        return;
    }

    odb::Rect rect = block->getDieArea();
    die_lx = rect.xMin();
    die_ly = rect.yMin();
    die_ux = rect.xMax();
    die_uy = rect.yMax();
}

void RouterDB::buildRoutingLayers(odb::dbTech* tech,
                                  odb::dbBlock* /*block*/,
                                  const std::vector<std::string>& lef_files)
{
    routing_layers.clear();
    via_resistance_from_db_.clear();
    has_hb_layer_resistance_from_db_ = false;
    hb_layer_resistance_from_db_ = 0.0;

    if (tech == nullptr) {
        return;
    }

    for (auto* layer : tech->getLayers()) {
        if (layer == nullptr) {
            continue;
        }

        if (layer->getRoutingLevel() <= 0) {
            continue;
        }

        LayerInfo info;
        info.name = layer->getName();
        info.routing_level = layer->getRoutingLevel();
        info.width = layer->getWidth();
        info.spacing = layer->getSpacing();
        info.pitch = layer->getPitch();
        info.offset = layer->getOffset();

        auto dir = layer->getDirection();
        info.is_horizontal = (dir == odb::dbTechLayerDir::HORIZONTAL);
        info.is_vertical   = (dir == odb::dbTechLayerDir::VERTICAL);

        // Preferred path: read RC/geometry from OpenDB tech layer APIs.
        info.resistance_rpersq = layer->getResistance();
        info.capacitance_cpersqdist = layer->getCapacitance();
        info.edge_capacitance = layer->getEdgeCapacitance();
        uint32_t layer_thickness = 0;
        if (layer->getThickness(layer_thickness)) {
            info.thickness = static_cast<double>(layer_thickness);
        }
        // NOTE: some OpenDB versions do not expose dbTechLayer::getHeight().
        // Keep height extraction in LEF fallback parser only.

        info.has_resistance = (info.resistance_rpersq > 0.0);
        info.has_capacitance = (info.capacitance_cpersqdist > 0.0);
        info.has_edge_capacitance = (info.edge_capacitance > 0.0);

        routing_layers.push_back(info);
    }

    std::sort(routing_layers.begin(),
              routing_layers.end(),
              [](const LayerInfo& a, const LayerInfo& b) {
                  return a.routing_level < b.routing_level;
              });

    // Fallback path: if OpenDB fields are absent/zero, parse LEF text and only fill missing fields.
    (void) extractLayerRCFromLefText(lef_files);
    applyLayerRCFallbackDefaults();
    (void) extractViaResistanceFromLefText(lef_files);
}

void RouterDB::applyLayerRCFallbackDefaults()
{
    for (auto& layer : routing_layers) {
        const auto rc = RCDefaults::findLayerRC(layer.name);
        if (!rc.has_value()) {
            continue;
        }

        bool touched = false;
        if (!isValidPositive(layer.resistance_rpersq) || !layer.has_resistance) {
            layer.resistance_rpersq = rc->resistance;
            layer.has_resistance = isValidPositive(layer.resistance_rpersq);
            touched = true;
        }
        if (!isValidPositive(layer.capacitance_cpersqdist) || !layer.has_capacitance) {
            layer.capacitance_cpersqdist = rc->capacitance;
            layer.has_capacitance = isValidPositive(layer.capacitance_cpersqdist);
            touched = true;
        }

        if (touched) {
            std::cout << "[RouterDB][RC][fallback] layer=" << layer.name
                      << " R=" << layer.resistance_rpersq
                      << " C=" << layer.capacitance_cpersqdist
                      << " (DB missing/invalid)" << std::endl;
        }
    }
}

bool RouterDB::extractViaResistanceFromLefText(const std::vector<std::string>& lef_files)
{
    bool filled_any = false;
    for (const auto& lef : lef_files) {
        std::ifstream fin(lef);
        if (!fin.good()) {
            continue;
        }

        std::string line;
        bool in_via = false;
        bool in_layer = false;
        std::string curr_via;
        std::string curr_layer;

        while (std::getline(fin, line)) {
            const std::string norm = normalizeLefLine(line);
            if (norm.empty()) {
                continue;
            }
            const std::vector<std::string> toks = splitTokens(norm);
            if (toks.empty()) {
                continue;
            }
            const std::string head = toUpper(toks[0]);

            if (!in_via && !in_layer && head == "VIA" && toks.size() >= 2) {
                in_via = true;
                curr_via = toks[1];
                continue;
            }
            if (!in_via && !in_layer && head == "LAYER" && toks.size() >= 2) {
                in_layer = true;
                curr_layer = toks[1];
                continue;
            }

            if (in_via) {
                if (head == "END" && toks.size() >= 2 && toUpper(toks[1]) == toUpper(curr_via)) {
                    in_via = false;
                    curr_via.clear();
                    continue;
                }
                if (head == "RESISTANCE" && toks.size() >= 2) {
                    double r = 0.0;
                    if (parseDouble(toks[1], r) && isValidPositive(r)) {
                        via_resistance_from_db_[toUpper(curr_via)] = r;
                        filled_any = true;
                    }
                }
                continue;
            }

            if (in_layer) {
                if (head == "END" && toks.size() >= 2 && toUpper(toks[1]) == toUpper(curr_layer)) {
                    in_layer = false;
                    curr_layer.clear();
                    continue;
                }
                if (toUpper(curr_layer) != "HB_LAYER") {
                    continue;
                }
                if (head == "RESISTANCE" && toks.size() >= 3 && toUpper(toks[1]) == "RPERSQ") {
                    double r = 0.0;
                    if (parseDouble(toks[2], r) && isValidPositive(r)) {
                        hb_layer_resistance_from_db_ = r;
                        has_hb_layer_resistance_from_db_ = true;
                        filled_any = true;
                    }
                }
            }
        }
    }

    if (filled_any) {
        std::cout << "[RouterDB][RC] loaded via/hb resistance from LEF/DB text path." << std::endl;
    }
    return filled_any;
}

bool RouterDB::extractLayerRCFromLefText(const std::vector<std::string>& lef_files)
{
    if (routing_layers.empty() || lef_files.empty()) {
        return false;
    }

    std::unordered_map<std::string, LayerInfo*> layer_by_name;
    for (auto& l : routing_layers) {
        layer_by_name[toUpper(l.name)] = &l;
    }

    bool filled_any = false;

    for (const auto& lef : lef_files) {
        std::ifstream fin(lef);
        if (!fin.good()) {
            continue;
        }

        std::string line;
        bool in_layer = false;
        bool is_routing = false;
        std::string curr_name;

        int width = 0;
        int spacing = 0;
        int pitch = 0;
        int offset = 0;
        bool has_width = false;
        bool has_spacing = false;
        bool has_pitch = false;
        bool has_offset = false;

        double r = 0.0;
        double c = 0.0;
        double ec = 0.0;
        double t = 0.0;
        double h = 0.0;
        bool has_r = false;
        bool has_c = false;
        bool has_ec = false;
        bool has_t = false;
        bool has_h = false;

        auto flush_layer = [&]() {
            if (!in_layer || !is_routing) {
                return;
            }
            auto it = layer_by_name.find(toUpper(curr_name));
            if (it == layer_by_name.end() || it->second == nullptr) {
                return;
            }

            LayerInfo& dst = *it->second;

            if (has_width && dst.width <= 0) {
                dst.width = width;
                filled_any = true;
            }
            if (has_spacing && dst.spacing <= 0) {
                dst.spacing = spacing;
                filled_any = true;
            }
            if (has_pitch && dst.pitch <= 0) {
                dst.pitch = pitch;
                filled_any = true;
            }
            if (has_offset && dst.offset <= 0) {
                dst.offset = offset;
                filled_any = true;
            }

            if (has_r && !dst.has_resistance) {
                dst.resistance_rpersq = r;
                dst.has_resistance = true;
                filled_any = true;
            }
            if (has_c && !dst.has_capacitance) {
                dst.capacitance_cpersqdist = c;
                dst.has_capacitance = true;
                filled_any = true;
            }
            if (has_ec && !dst.has_edge_capacitance) {
                dst.edge_capacitance = ec;
                dst.has_edge_capacitance = true;
                filled_any = true;
            }
            if (has_t && dst.thickness <= 0.0) {
                dst.thickness = t;
                filled_any = true;
            }
            if (has_h && dst.height <= 0.0) {
                dst.height = h;
                filled_any = true;
            }
        };

        while (std::getline(fin, line)) {
            const std::string norm = normalizeLefLine(line);
            if (norm.empty()) {
                continue;
            }

            const std::vector<std::string> toks = splitTokens(norm);
            if (toks.empty()) {
                continue;
            }
            const std::string head = toUpper(toks[0]);

            if (!in_layer) {
                if (head == "LAYER" && toks.size() >= 2) {
                    in_layer = true;
                    is_routing = false;
                    curr_name = toks[1];

                    width = spacing = pitch = offset = 0;
                    has_width = has_spacing = has_pitch = has_offset = false;

                    r = c = ec = t = h = 0.0;
                    has_r = has_c = has_ec = has_t = has_h = false;
                }
                continue;
            }

            if (head == "END" && toks.size() >= 2 && toUpper(toks[1]) == toUpper(curr_name)) {
                flush_layer();
                in_layer = false;
                is_routing = false;
                curr_name.clear();
                continue;
            }

            if (head == "TYPE" && toks.size() >= 2) {
                is_routing = (toUpper(toks[1]) == "ROUTING");
                continue;
            }

            if (!is_routing) {
                continue;
            }

            if (head == "WIDTH" && toks.size() >= 2) {
                double v = 0.0;
                if (parseDouble(toks[1], v)) {
                    width = micronToDBU(v, dbu_per_micron);
                    has_width = true;
                }
                continue;
            }

            if (head == "SPACING" && toks.size() >= 2) {
                double v = 0.0;
                if (parseDouble(toks[1], v)) {
                    spacing = micronToDBU(v, dbu_per_micron);
                    has_spacing = true;
                }
                continue;
            }

            if (head == "PITCH" && toks.size() >= 2) {
                double v = 0.0;
                if (parseDouble(toks[1], v)) {
                    pitch = micronToDBU(v, dbu_per_micron);
                    has_pitch = true;
                }
                continue;
            }

            if (head == "OFFSET" && toks.size() >= 2) {
                double v = 0.0;
                if (parseDouble(toks[1], v)) {
                    offset = micronToDBU(v, dbu_per_micron);
                    has_offset = true;
                }
                continue;
            }

            if (head == "RESISTANCE" && toks.size() >= 3 && toUpper(toks[1]) == "RPERSQ") {
                if (parseDouble(toks[2], r)) {
                    has_r = true;
                }
                continue;
            }

            if (head == "CAPACITANCE" && toks.size() >= 3 && toUpper(toks[1]) == "CPERSQDIST") {
                if (parseDouble(toks[2], c)) {
                    has_c = true;
                }
                continue;
            }

            if (head == "EDGECAPACITANCE" && toks.size() >= 2) {
                if (parseDouble(toks[1], ec)) {
                    has_ec = true;
                }
                continue;
            }

            if (head == "THICKNESS" && toks.size() >= 2) {
                if (parseDouble(toks[1], t)) {
                    has_t = true;
                }
                continue;
            }

            if (head == "HEIGHT" && toks.size() >= 2) {
                if (parseDouble(toks[1], h)) {
                    has_h = true;
                }
                continue;
            }
        }

        // handle malformed/unterminated block at EOF
        flush_layer();
    }

    if (filled_any) {
        std::cout << "[RouterDB] filled missing routing RC/geometry fields from LEF fallback parser.\n";
    }
    return filled_any;
}

EffectiveRC RouterDB::computeEffectiveRCForDie(DieId die) const
{
    // Current global routing phase uses a domain-level equivalent RC average.
    // This keeps timing-driven cost non-zero before true per-segment layer assignment exists.
    // Future extension: when each segment has concrete layer assignment, use LayerInfo RC directly.
    auto accumulate = [&](bool only_domain, bool need_top) {
        double sum_r = 0.0;
        double sum_c = 0.0;
        int cnt_r = 0;
        int cnt_c = 0;

        for (const auto& layer : routing_layers) {
            bool in_domain = true;
            if (only_domain) {
                if (need_top) {
                    in_domain = isTopName(layer.name);
                } else {
                    in_domain = isBottomName(layer.name);
                }
            }

            if (!in_domain) {
                continue;
            }

            if (layer.has_resistance) {
                sum_r += layer.resistance_rpersq;
                ++cnt_r;
            }
            if (layer.has_capacitance) {
                // Keep current model simple: CPERSQDIST as wire unit capacitance proxy.
                // Edge capacitance can be folded in later when segment geometry/layer is known.
                sum_c += layer.capacitance_cpersqdist;
                ++cnt_c;
            }
        }

        EffectiveRC rc;
        rc.unit_res = (cnt_r > 0) ? (sum_r / static_cast<double>(cnt_r)) : 0.0;
        rc.unit_cap = (cnt_c > 0) ? (sum_c / static_cast<double>(cnt_c)) : 0.0;
        return rc;
    };

    const bool want_top = (die == DieId::kTop);
    EffectiveRC rc = accumulate(true, want_top);
    if (isValidPositive(rc.unit_res) && isValidPositive(rc.unit_cap)) {
        return rc;
    }

    // Domain has no valid RC -> fallback to average of all routing layers.
    rc = accumulate(false, want_top);
    if (!isValidPositive(rc.unit_res) || !isValidPositive(rc.unit_cap)) {
        const EffectiveRC signal = getSignalWireRCFallback();
        if (!isValidPositive(rc.unit_res)) {
            rc.unit_res = signal.unit_res;
        }
        if (!isValidPositive(rc.unit_cap)) {
            rc.unit_cap = signal.unit_cap;
        }
        std::cout << "[RouterDB][RC][fallback] effective "
                  << (want_top ? "top" : "bottom")
                  << " domain uses signal-wire default R/C="
                  << rc.unit_res << "/" << rc.unit_cap << std::endl;
    }
    return rc;
}

EffectiveRC RouterDB::computeDirectionalRCForDie(DieId die, bool horizontal_preferred) const
{
    auto accumulate = [&](bool only_domain, bool only_direction) {
        double sum_r = 0.0;
        double sum_c = 0.0;
        int cnt_r = 0;
        int cnt_c = 0;
        const bool need_top = (die == DieId::kTop);
        for (const auto& layer : routing_layers) {
            if (only_domain) {
                const bool in_domain = need_top ? isTopName(layer.name) : isBottomName(layer.name);
                if (!in_domain) {
                    continue;
                }
            }

            if (only_direction) {
                const bool matches_dir = horizontal_preferred ? layer.is_horizontal : layer.is_vertical;
                if (!matches_dir) {
                    continue;
                }
            }

            if (layer.has_resistance && isValidPositive(layer.resistance_rpersq)) {
                sum_r += layer.resistance_rpersq;
                ++cnt_r;
            }
            if (layer.has_capacitance && isValidPositive(layer.capacitance_cpersqdist)) {
                sum_c += layer.capacitance_cpersqdist;
                ++cnt_c;
            }
        }

        EffectiveRC rc;
        rc.unit_res = (cnt_r > 0) ? (sum_r / static_cast<double>(cnt_r)) : 0.0;
        rc.unit_cap = (cnt_c > 0) ? (sum_c / static_cast<double>(cnt_c)) : 0.0;
        return rc;
    };

    // NOTE:
    //   This is a global-routing layer-aware proxy (direction-preferred average),
    //   not detailed per-segment final layer assignment.
    EffectiveRC rc = accumulate(true, true);
    if (isValidPositive(rc.unit_res) && isValidPositive(rc.unit_cap)) {
        return rc;
    }
    rc = accumulate(true, false);
    if (isValidPositive(rc.unit_res) && isValidPositive(rc.unit_cap)) {
        return rc;
    }
    rc = accumulate(false, true);
    if (isValidPositive(rc.unit_res) && isValidPositive(rc.unit_cap)) {
        return rc;
    }

    const EffectiveRC signal = getSignalWireRCFallback();
    if (!isValidPositive(rc.unit_res)) {
        rc.unit_res = signal.unit_res;
    }
    if (!isValidPositive(rc.unit_cap)) {
        rc.unit_cap = signal.unit_cap;
    }
    if (isValidPositive(rc.unit_res) && isValidPositive(rc.unit_cap)) {
        return rc;
    }

    return computeEffectiveRCForDie(die);
}

EffectiveRC RouterDB::getDefaultWireRCForNetClass(NetClass cls) const
{
    const DefaultWireRC rc = RCDefaults::wireRCForNetClass(cls);
    return EffectiveRC{rc.resistance, rc.capacitance};
}

EffectiveRC RouterDB::getSignalWireRCFallback() const
{
    return getDefaultWireRCForNetClass(NetClass::kNormal);
}

EffectiveRC RouterDB::getClockWireRCFallback() const
{
    return getDefaultWireRCForNetClass(NetClass::kClock);
}

double RouterDB::getViaResistanceOrDefault(const std::string& via_name) const
{
    const std::string key = toUpper(via_name);
    if (key == "HB_LAYER" && has_hb_layer_resistance_from_db_ && isValidPositive(hb_layer_resistance_from_db_)) {
        return hb_layer_resistance_from_db_;
    }
    const auto it = via_resistance_from_db_.find(key);
    if (it != via_resistance_from_db_.end() && isValidPositive(it->second)) {
        return it->second;
    }

    const auto fallback = RCDefaults::findViaResistance(via_name);
    if (fallback.has_value() && isValidPositive(*fallback)) {
        return *fallback;
    }
    return 0.0;
}

double RouterDB::getHBTResistanceOrDefault() const
{
    if (has_hb_layer_resistance_from_db_ && isValidPositive(hb_layer_resistance_from_db_)) {
        return hb_layer_resistance_from_db_;
    }

    if (!hbt.hb_layer_name.empty()) {
        const auto named = RCDefaults::findViaResistance(hbt.hb_layer_name);
        if (named.has_value() && isValidPositive(*named)) {
            return *named;
        }
    }

    const auto fallback = RCDefaults::findViaResistance("hb_layer");
    return (fallback.has_value() && isValidPositive(*fallback)) ? *fallback : 0.0;
}

std::vector<std::string> RouterDB::getTopGuideCandidateLayers() const
{
    std::vector<std::string> out;
    for (const auto& layer : routing_layers) {
        const std::string up = toUpper(layer.name);
        if (up.empty() || up.find("HB") != std::string::npos) {
            continue;
        }
        // Top domain keeps base stack names; avoid lower-die aliases.
        if (up.find("_M") != std::string::npos || up.find("_ADD") != std::string::npos
            || up.find("BOTTOM") != std::string::npos || up.find("LOWER") != std::string::npos) {
            continue;
        }
        out.push_back(layer.name);
    }
    return out;
}

std::vector<std::string> RouterDB::getBottomGuideCandidateLayers() const
{
    std::vector<std::string> out;
    for (const auto& layer : routing_layers) {
        const std::string up = toUpper(layer.name);
        if (up.empty() || up.find("HB") != std::string::npos) {
            continue;
        }
        if (up.find("_M") != std::string::npos || up.find("_ADD") != std::string::npos
            || up.find("BOTTOM") != std::string::npos || up.find("LOWER") != std::string::npos) {
            out.push_back(layer.name);
        }
    }
    return out;
}

std::vector<const Net*> RouterDB::getRouteableNets() const
{
    std::vector<const Net*> out;
    out.reserve(nets.size());
    for (const auto& net : nets) {
        if (net.net_class == NetClass::kSpecial) {
            continue;
        }
        out.push_back(&net);
    }
    return out;
}

const std::vector<Pin>* RouterDB::getPins(const std::string& net_name) const
{
    const auto it = net_name_to_id_.find(net_name);
    if (it == net_name_to_id_.end()) {
        return nullptr;
    }
    return &nets[it->second].pins;
}

const std::vector<PinShape>* RouterDB::getPinShapes(const std::string& net_name,
                                                    const std::string& pin_name) const
{
    const auto* pins = getPins(net_name);
    if (pins == nullptr) {
        return nullptr;
    }
    for (const auto& pin : *pins) {
        if (pin.name == pin_name) {
            return &pin.shapes;
        }
    }
    return nullptr;
}

const LayerInfo* RouterDB::getRoutingLayer(const std::string& layer_name) const
{
    const std::string up = toUpper(layer_name);
    for (const auto& layer : routing_layers) {
        if (toUpper(layer.name) == up) {
            return &layer;
        }
    }
    return nullptr;
}

std::string RouterDB::getLayerDirection(const std::string& layer_name) const
{
    const LayerInfo* l = getRoutingLayer(layer_name);
    if (l == nullptr) {
        return "unknown";
    }
    if (l->is_horizontal) {
        return "horizontal";
    }
    if (l->is_vertical) {
        return "vertical";
    }
    return "unknown";
}

int RouterDB::getLayerLevel(const std::string& layer_name) const
{
    const LayerInfo* l = getRoutingLayer(layer_name);
    return (l == nullptr) ? -1 : l->routing_level;
}

std::vector<std::string> RouterDB::getAdjacentLayers(const std::string& layer_name) const
{
    std::vector<std::string> out;
    const int lv = getLayerLevel(layer_name);
    if (lv < 0) {
        return out;
    }
    for (const auto& l : routing_layers) {
        if (std::abs(l.routing_level - lv) == 1) {
            out.push_back(l.name);
        }
    }
    if (isHBLayer(layer_name)) {
        for (const auto& l : routing_layers) {
            if (!isHBLayer(l.name) && (l.routing_level >= 6 || l.routing_level >= 10)) {
                out.push_back(l.name);
            }
        }
    }
    return out;
}

bool RouterDB::hasTrackInBox(const std::string& layer_name, const Box2D& box) const
{
    const LayerInfo* l = getRoutingLayer(layer_name);
    if (l == nullptr || !box.valid()) {
        return false;
    }
    if (!l->has_track_grid || l->pitch <= 0) {
        return true;
    }
    const int step = l->pitch;
    if (l->is_horizontal) {
        const int origin = l->track_init_y;
        const int lo = std::min(box.ly, box.uy);
        const int hi = std::max(box.ly, box.uy);
        if (hi < origin) {
            return false;
        }
        const int first = (lo <= origin) ? origin : (origin + ((lo - origin + step - 1) / step) * step);
        return first <= hi;
    }
    if (l->is_vertical) {
        const int origin = l->track_init_x;
        const int lo = std::min(box.lx, box.ux);
        const int hi = std::max(box.lx, box.ux);
        if (hi < origin) {
            return false;
        }
        const int first = (lo <= origin) ? origin : (origin + ((lo - origin + step - 1) / step) * step);
        return first <= hi;
    }
    return true;
}

NetClass RouterDB::classifyNet(const Net& net) const
{
    return net.net_class;
}

std::string RouterDB::getPreferredHorizontalLayer(DieId die) const
{
    const auto candidates = (die == DieId::kBottom)
                                ? getBottomGuideCandidateLayers()
                                : getTopGuideCandidateLayers();
    for (const auto& name : candidates) {
        for (const auto& layer : routing_layers) {
            if (layer.name == name && layer.is_horizontal) {
                return name;
            }
        }
    }
    return candidates.empty() ? std::string() : candidates.front();
}

std::string RouterDB::getPreferredVerticalLayer(DieId die) const
{
    const auto candidates = (die == DieId::kBottom)
                                ? getBottomGuideCandidateLayers()
                                : getTopGuideCandidateLayers();
    for (const auto& name : candidates) {
        for (const auto& layer : routing_layers) {
            if (layer.name == name && layer.is_vertical) {
                return name;
            }
        }
    }
    return candidates.empty() ? std::string() : candidates.front();
}

bool RouterDB::isKnownRoutingLayer(const std::string& layer_name) const
{
    const std::string up = toUpper(layer_name);
    for (const auto& layer : routing_layers) {
        if (toUpper(layer.name) == up) {
            return true;
        }
    }
    return false;
}

bool RouterDB::isHBLayer(const std::string& layer_name) const
{
    const std::string up = toUpper(layer_name);
    if (up.empty()) {
        return false;
    }
    if (!hbt.hb_layer_name.empty() && up == toUpper(hbt.hb_layer_name)) {
        return true;
    }
    return up.find("HB") != std::string::npos;
}

double RouterDB::computeEffectiveTopUnitRes() const
{
    return computeEffectiveRCForDie(DieId::kTop).unit_res;
}

double RouterDB::computeEffectiveTopUnitCap() const
{
    return computeEffectiveRCForDie(DieId::kTop).unit_cap;
}

double RouterDB::computeEffectiveBottomUnitRes() const
{
    return computeEffectiveRCForDie(DieId::kBottom).unit_res;
}

double RouterDB::computeEffectiveBottomUnitCap() const
{
    return computeEffectiveRCForDie(DieId::kBottom).unit_cap;
}

double RouterDB::dbuToMicronLength(int length_dbu) const
{
    if (dbu_per_micron > 0) {
        return static_cast<double>(length_dbu) / static_cast<double>(dbu_per_micron);
    }
    return static_cast<double>(length_dbu);
}

DieId RouterDB::inferDieFromMasterOrInst(const std::string& master_name,
                                         const std::string& inst_name) const
{
    if (isTopName(master_name) || isTopName(inst_name)) {
        return DieId::kTop;
    }
    if (isBottomName(master_name) || isBottomName(inst_name)) {
        return DieId::kBottom;
    }
    return DieId::kUnknown;
}

void RouterDB::buildNodes(odb::dbBlock* block)
{
    if (block == nullptr) {
        return;
    }

    int node_id = 0;

    for (auto* inst : block->getInsts()) {
        if (inst == nullptr) {
            continue;
        }

        Node node;
        node.id = node_id++;
        node.name = inst->getName();
        node.fixed = inst->isFixed();

        odb::dbMaster* master = inst->getMaster();
        if (master != nullptr) {
            node.master_name = master->getName();
            node.width = static_cast<int>(master->getWidth());
            node.height = static_cast<int>(master->getHeight());
        }

        odb::Point loc = inst->getLocation();
        node.x = loc.x();
        node.y = loc.y();
        node.die = inferDieFromMasterOrInst(node.master_name, node.name);

        node_name_to_id[node.name] = node.id;
        nodes.push_back(node);
    }
}

Point2D RouterDB::getITermPinLocation(odb::dbITerm* iterm) const
{
    Point2D p{0, 0};

    if (iterm == nullptr) {
        return p;
    }

    int x = 0;
    int y = 0;

    if (iterm->getAvgXY(&x, &y)) {
        p.x = x;
        p.y = y;
        return p;
    }

    odb::Rect bbox = iterm->getBBox();
    if (bbox.xMin() != bbox.xMax() || bbox.yMin() != bbox.yMax()) {
        p.x = (bbox.xMin() + bbox.xMax()) / 2;
        p.y = (bbox.yMin() + bbox.yMax()) / 2;
        return p;
    }

    odb::dbInst* inst = iterm->getInst();
    if (inst != nullptr) {
        odb::Point loc = inst->getLocation();
        p.x = loc.x();
        p.y = loc.y();

        odb::dbMaster* master = inst->getMaster();
        if (master != nullptr) {
            p.x += static_cast<int>(master->getWidth()) / 2;
            p.y += static_cast<int>(master->getHeight()) / 2;
        }
    }

    return p;
}

Point2D RouterDB::getBTermPinLocation(odb::dbBTerm* bterm) const
{
    Point2D p{0, 0};

    if (bterm == nullptr) {
        return p;
    }

    int x = 0;
    int y = 0;
    if (bterm->getFirstPinLocation(x, y)) {
        p.x = x;
        p.y = y;
        return p;
    }

    odb::Rect bbox = bterm->getBBox();
    p.x = (bbox.xMin() + bbox.xMax()) / 2;
    p.y = (bbox.yMin() + bbox.yMax()) / 2;
    return p;
}

Box2D RouterDB::getITermPinBBox(odb::dbITerm* iterm) const
{
    Box2D box;
    if (iterm == nullptr) {
        return box;
    }
    const odb::Rect bbox = iterm->getBBox();
    box.lx = bbox.xMin();
    box.ly = bbox.yMin();
    box.ux = bbox.xMax();
    box.uy = bbox.yMax();
    if (!box.valid()) {
        const Point2D p = getITermPinLocation(iterm);
        box.lx = p.x;
        box.ly = p.y;
        box.ux = p.x + 1;
        box.uy = p.y + 1;
    }
    return box;
}

Box2D RouterDB::getBTermPinBBox(odb::dbBTerm* bterm) const
{
    Box2D box;
    if (bterm == nullptr) {
        return box;
    }
    const odb::Rect bbox = bterm->getBBox();
    box.lx = bbox.xMin();
    box.ly = bbox.yMin();
    box.ux = bbox.xMax();
    box.uy = bbox.yMax();
    if (!box.valid()) {
        const Point2D p = getBTermPinLocation(bterm);
        box.lx = p.x;
        box.ly = p.y;
        box.ux = p.x + 1;
        box.uy = p.y + 1;
    }
    return box;
}

std::vector<PinShape> RouterDB::collectITermPinShapes(odb::dbITerm* iterm) const
{
    std::vector<PinShape> shapes;
    if (iterm == nullptr) {
        return shapes;
    }

    odb::dbMTerm* mterm = iterm->getMTerm();
    odb::dbInst* inst = iterm->getInst();
    if (inst == nullptr || mterm == nullptr) {
        return shapes;
    }

    const odb::Rect bbox = iterm->getBBox();
    PinShape shape;
    shape.layer = inferITermPinLayerName(mterm);
    shape.bbox.lx = bbox.xMin();
    shape.bbox.ly = bbox.yMin();
    shape.bbox.ux = bbox.xMax();
    shape.bbox.uy = bbox.yMax();
    if (shape.bbox.valid()) {
        shape.source = "iterm_bbox";
        shapes.push_back(shape);
        return shapes;
    }

    int x = 0;
    int y = 0;
    if (iterm->getAvgXY(&x, &y)) {
        constexpr int kHalf = 100;
        shape.bbox.lx = x - kHalf;
        shape.bbox.ly = y - kHalf;
        shape.bbox.ux = x + kHalf;
        shape.bbox.uy = y + kHalf;
        shape.source = "iterm_avgxy_fallback";
        shapes.push_back(shape);
        emitRouterDBWarning("ITerm " + iterm->getName()
                            + " has invalid bbox; using getAvgXY fallback.");
        return shapes;
    }

    const Point2D p = getITermPinLocation(iterm);
    shape.bbox.lx = p.x - 1;
    shape.bbox.ly = p.y - 1;
    shape.bbox.ux = p.x + 1;
    shape.bbox.uy = p.y + 1;
    shape.source = "iterm_avgxy_fallback";
    shapes.push_back(shape);
    emitRouterDBWarning("ITerm " + iterm->getName()
                        + " has no valid bbox/getAvgXY; using point fallback.");
    return shapes;
}

std::string RouterDB::inferITermPinLayerName(odb::dbMTerm* mterm) const
{
    if (mterm != nullptr) {
        for (odb::dbMPin* mpin : mterm->getMPins()) {
            if (mpin == nullptr) {
                continue;
            }
            for (odb::dbBox* box : mpin->getGeometry()) {
                if (box == nullptr) {
                    continue;
                }
                odb::dbTechLayer* layer = box->getTechLayer();
                if (layer != nullptr && layer->getType() == odb::dbTechLayerType::ROUTING) {
                    return layer->getName();
                }
            }
        }
    }

    auto layerExists = [this](const std::string& name) {
        return isKnownRoutingLayer(name);
    };
    auto isBottomLayerName = [](const std::string& name) {
        return containsInsensitive(name, "_M");
    };

    bool prefer_bottom = false;
    if (mterm != nullptr && mterm->getMaster() != nullptr) {
        const std::string master_name = mterm->getMaster()->getName();
        prefer_bottom = isBottomName(master_name) && !isTopName(master_name);
    }

    const std::vector<std::string> preferred =
        prefer_bottom ? std::vector<std::string>{"M1_m", "M2_m", "M1", "M2"}
                      : std::vector<std::string>{"M1", "M2", "M1_m", "M2_m"};
    for (const auto& name : preferred) {
        if (layerExists(name)) {
            return name;
        }
    }

    const LayerInfo* best = nullptr;
    for (const auto& layer : routing_layers) {
        if (prefer_bottom != isBottomLayerName(layer.name)) {
            continue;
        }
        if (best == nullptr || layer.routing_level < best->routing_level) {
            best = &layer;
        }
    }
    if (best != nullptr) {
        return best->name;
    }

    if (!routing_layers.empty()) {
        return routing_layers.front().name;
    }
    return "";
}

std::vector<PinShape> RouterDB::collectBTermPinShapes(odb::dbBTerm* bterm) const
{
    std::vector<PinShape> shapes;
    if (bterm == nullptr) {
        return shapes;
    }
    PinShape s;
    s.bbox = getBTermPinBBox(bterm);
    shapes.push_back(s);
    return shapes;
}

void RouterDB::refreshNetClass(Net& net) const
{
    if (net.net_class == NetClass::kSpecial) {
        return;
    }
    if (net.net_class == NetClass::kClock) {
        return;
    }
    if (net.top_pin_count > 0 && net.bottom_pin_count > 0) {
        net.net_class = NetClass::kMixed;
    } else if (net.top_pin_count > 0) {
        net.net_class = NetClass::kUpperOnly;
    } else if (net.bottom_pin_count > 0) {
        net.net_class = NetClass::kBottomOnly;
    } else {
        net.net_class = NetClass::kUnknown;
    }
}

void RouterDB::buildNets(odb::dbBlock* block)
{
    if (block == nullptr) {
        return;
    }

    for (auto* db_net : block->getNets()) {
        if (db_net == nullptr) {
            continue;
        }

        Net net;
        net.name = db_net->getName();
        net.is_special = db_net->isSpecial();
        net.net_class = classifyNetFromSignalType(db_net, net.name, net.is_special);
        net.is_clock = (net.net_class == NetClass::kClock);

        for (auto* iterm : db_net->getITerms()) {
            if (iterm == nullptr) {
                continue;
            }

            Pin pin;
            pin.is_bterm = false;

            odb::dbInst* inst = iterm->getInst();
            odb::dbMTerm* mterm = iterm->getMTerm();

            if (inst != nullptr) {
                pin.node_name = inst->getName();
                auto found = node_name_to_id.find(pin.node_name);
                pin.node_id = (found == node_name_to_id.end()) ? -1 : found->second;

                if (pin.node_id >= 0) {
                    pin.die = nodes[pin.node_id].die;
                }
            }

            pin.name = (mterm != nullptr) ? mterm->getName() : "ITERM";

            Point2D loc = getITermPinLocation(iterm);
            pin.x = loc.x;
            pin.y = loc.y;
            pin.bbox = getITermPinBBox(iterm);
            pin.shapes = collectITermPinShapes(iterm);

            if (pin.die == DieId::kTop) {
                ++net.top_pin_count;
            } else if (pin.die == DieId::kBottom) {
                ++net.bottom_pin_count;
            }

            net.pins.push_back(pin);
        }

        for (auto* bterm : db_net->getBTerms()) {
            if (bterm == nullptr) {
                continue;
            }

            Pin pin;
            pin.node_id = -1;
            pin.node_name = "PORT";
            pin.name = bterm->getName();
            pin.is_bterm = true;
            pin.die = DieId::kUnknown;

            Point2D loc = getBTermPinLocation(bterm);
            pin.x = loc.x;
            pin.y = loc.y;
            pin.bbox = getBTermPinBBox(bterm);
            pin.shapes = collectBTermPinShapes(bterm);

            net.pins.push_back(pin);
        }

        net.is_3d = (net.top_pin_count > 0 && net.bottom_pin_count > 0);
        finalizeNetPinDies(net);
        refreshNetClass(net);
        net_name_to_id_[net.name] = static_cast<int>(nets.size());
        nets.push_back(net);
    }
}

DieId RouterDB::inferNetDominantDieFor2D(const Net& net) const
{
    if (net.top_pin_count > 0 && net.bottom_pin_count == 0) {
        return DieId::kTop;
    }
    if (net.bottom_pin_count > 0 && net.top_pin_count == 0) {
        return DieId::kBottom;
    }
    return DieId::kUnknown;
}

void RouterDB::finalizeNetPinDies(Net& net) const
{
    // Unknown die must never leak into routing topology. We resolve all pins,
    // including 3D/mixed nets, using instance die + access-layer heuristics.
    auto inferFromLayer = [](const std::string& layer_name) {
        if (layer_name.empty()) {
            return DieId::kUnknown;
        }
        if (containsInsensitive(layer_name, "_M")
            || containsInsensitive(layer_name, "BOTTOM")
            || containsInsensitive(layer_name, "LOWER")) {
            return DieId::kBottom;
        }
        if (layer_name.size() >= 2
            && (layer_name[0] == 'M' || layer_name[0] == 'm')
            && std::isdigit(static_cast<unsigned char>(layer_name[1])) != 0) {
            return DieId::kTop;
        }
        return DieId::kUnknown;
    };

    int top_known = 0;
    int bottom_known = 0;
    for (const auto& pin : net.pins) {
        if (pin.die == DieId::kTop) {
            ++top_known;
        } else if (pin.die == DieId::kBottom) {
            ++bottom_known;
        }
    }
    const DieId majority_die = (top_known >= bottom_known) ? DieId::kTop : DieId::kBottom;

    for (auto& pin : net.pins) {
        if (pin.die != DieId::kUnknown) {
            continue;
        }
        DieId inferred = DieId::kUnknown;
        if (pin.node_id >= 0 && pin.node_id < static_cast<int>(nodes.size())) {
            inferred = nodes[pin.node_id].die;
        }
        if (inferred == DieId::kUnknown) {
            inferred = inferFromLayer(pin.access_layer);
        }
        if (inferred == DieId::kUnknown) {
            inferred = majority_die;
            std::cerr << "[RouterDB] warning: fallback infer die by majority net=" << net.name
                      << " pin=" << pin.name << "\n";
        }
        pin.die = inferred;
        std::cout << "[RouterDB] finalize pin die net=" << net.name
                  << " pin=" << pin.name
                  << " Unknown->" << dieToString(inferred) << std::endl;
    }
}

void RouterDB::extractHBTInfo(odb::dbDatabase* db,
                              const std::vector<std::string>& lef_files)
{
    hbt = {};

    if (db != nullptr && db->getTech() != nullptr) {
        odb::dbTech* tech = db->getTech();

        odb::dbTechLayer* hb_layer = tech->findLayer("hb_layer");
        if (hb_layer != nullptr) {
            hbt.found = true;
            hbt.hb_layer_name = hb_layer->getName();
            const double hb_r = hb_layer->getResistance();
            if (isValidPositive(hb_r)) {
                hb_layer_resistance_from_db_ = hb_r;
                has_hb_layer_resistance_from_db_ = true;
                std::cout << "[RouterDB][RC] hb_layer resistance from DB tech="
                          << hb_layer_resistance_from_db_ << std::endl;
            }
        }

        odb::dbTechViaGenerateRule* hb_rule = tech->findViaGenerateRule("hb_layerArray-0");
        if (hb_rule != nullptr) {
            hbt.found = true;
            hbt.hb_viarule_name = hb_rule->getName();

            uint32_t cnt = hb_rule->getViaLayerRuleCount();
            for (uint32_t i = 0; i < cnt; ++i) {
                odb::dbTechViaLayerRule* lrule = hb_rule->getViaLayerRule(i);
                if (lrule == nullptr || lrule->getLayer() == nullptr) {
                    continue;
                }

                const std::string lname = lrule->getLayer()->getName();
                if (!containsInsensitive(lname, "HB")) {
                    continue;
                }

                if (lrule->hasRect()) {
                    odb::Rect r;
                    lrule->getRect(r);
                    hbt.cut_width = r.dx();
                    hbt.cut_height = r.dy();
                }

                if (lrule->hasSpacing()) {
                    int sx = 0;
                    int sy = 0;
                    lrule->getSpacing(sx, sy);
                    hbt.spacing_x = sx;
                    hbt.spacing_y = sy;
                }

                hbt.pitch_x = hbt.cut_width + hbt.spacing_x;
                hbt.pitch_y = hbt.cut_height + hbt.spacing_y;
                return;
            }
        }
    }

    // fallback：若 OpenDB 未完整拿到规则，则扫 LEF 文本
    for (const auto& lef : lef_files) {
        std::ifstream fin(lef);
        if (!fin.good()) {
            continue;
        }

        std::string line;
        bool in_hb_rule = false;

        while (std::getline(fin, line)) {
            std::string up = toUpper(line);

            if (up.find("LAYER HB_LAYER") != std::string::npos) {
                hbt.found = true;
                hbt.hb_layer_name = "hb_layer";
            }

            if (up.find("VIARULE HB_LAYERARRAY-0") != std::string::npos) {
                hbt.found = true;
                hbt.hb_viarule_name = "hb_layerArray-0";
                in_hb_rule = true;
                continue;
            }

            if (in_hb_rule && up.find("END") != std::string::npos) {
                in_hb_rule = false;
            }

            if (!in_hb_rule) {
                continue;
            }

            if (up.find("RECT") != std::string::npos) {
                std::stringstream ss(line);
                std::string tok;
                int x1, y1, x2, y2;
                if (ss >> tok >> x1 >> y1 >> x2 >> y2) {
                    hbt.cut_width = std::abs(x2 - x1);
                    hbt.cut_height = std::abs(y2 - y1);
                }
            }

            if (up.find("SPACING") != std::string::npos) {
                std::stringstream ss(line);
                std::string tok1, tok2;
                int sx, sy;
                if (ss >> tok1 >> sx >> tok2 >> sy) {
                    hbt.spacing_x = sx;
                    hbt.spacing_y = sy;
                }
            }
        }

        if (hbt.cut_width > 0 || hbt.spacing_x > 0) {
            hbt.pitch_x = hbt.cut_width + hbt.spacing_x;
        }
        if (hbt.cut_height > 0 || hbt.spacing_y > 0) {
            hbt.pitch_y = hbt.cut_height + hbt.spacing_y;
        }

        if (hbt.found) {
            return;
        }
    }
}

int RouterDB::get3DNetCount() const
{
    int cnt = 0;
    for (const auto& net : nets) {
        if (net.is_3d) {
            ++cnt;
        }
    }
    return cnt;
}

int RouterDB::get2DNetCount() const
{
    int cnt = 0;
    for (const auto& net : nets) {
        if (!net.is_3d) {
            ++cnt;
        }
    }
    return cnt;
}
