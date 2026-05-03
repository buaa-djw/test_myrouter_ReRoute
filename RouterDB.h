#pragma once

#include "odb/db.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class DieId
{
    kUnknown = -1,
    kTop = 0,
    kBottom = 1
};

enum class PinDir
{
    kUnknown = 0,
    kInput,
    kOutput,
    kInout
};

struct Point2D
{
    int x = 0;
    int y = 0;
};

struct Box2D
{
    int lx = 0;
    int ly = 0;
    int ux = 0;
    int uy = 0;

    bool valid() const { return ux > lx && uy > ly; }
};

struct PinShape
{
    Box2D bbox;
    std::string layer;
    std::string source;
};

struct LayerInfo
{
    std::string name;
    int routing_level = -1;

    bool is_horizontal = false;
    bool is_vertical = false;

    int width = 0;
    int spacing = 0;
    int pitch = 0;
    int offset = 0;

    int track_init_x = 0;
    int track_init_y = 0;
    int track_step_x = 0;
    int track_step_y = 0;
    int num_tracks_x = 0;
    int num_tracks_y = 0;
    bool has_track_grid = false;

    // routing layer RC/geometry from OpenDB tech layer (fallback to LEF text parser)
    double resistance_rpersq = 0.0;
    double capacitance_cpersqdist = 0.0;
    double edge_capacitance = 0.0;
    double thickness = 0.0;
    double height = 0.0;

    bool has_resistance = false;
    bool has_capacitance = false;
    bool has_edge_capacitance = false;
};

struct EffectiveRC
{
    double unit_res = 0.0;
    double unit_cap = 0.0;
};

struct HBTInfo
{
    bool found = false;
    std::string hb_layer_name;
    std::string hb_viarule_name;

    int cut_width = 0;
    int cut_height = 0;
    int spacing_x = 0;
    int spacing_y = 0;
    int pitch_x = 0;
    int pitch_y = 0;

    bool has_parasitic = false;
    double parasitic_res = 0.0;
    double parasitic_cap = 0.0;
};

struct Node
{
    int id = -1;
    std::string name;
    std::string master_name;

    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool fixed = false;

    DieId die = DieId::kUnknown;
};

struct Pin
{
    int node_id = -1;     // -1 表示 bterm
    std::string node_name;
    std::string name;
    int x = 0;
    int y = 0;
    std::string access_layer;
    bool is_bterm = false;

    DieId die = DieId::kUnknown;
    PinDir dir = PinDir::kUnknown;
    double input_cap = 0.0;
    bool has_input_cap = false;
    bool is_clock_pin = false;
    Box2D bbox;
    std::vector<PinShape> shapes;
};

enum class NetClass
{
    kNormal = 0,
    kUpperOnly,
    kBottomOnly,
    kMixed,
    kClock,
    kSpecial,
    kUnknown
};

// Helper for summary/logging of net classification.
const char* netClassToString(NetClass c);

struct Net
{
    std::string name;
    std::vector<Pin> pins;

    bool is_special = false;
    bool is_clock = false;
    NetClass net_class = NetClass::kNormal;
    bool is_3d = false;

    int top_pin_count = 0;
    int bottom_pin_count = 0;

    int source_pin_index = -1;
    bool source_from_verilog = false;
    std::string skip_reason;

    // Only normal signal nets enter current PDTreeRouter flow.
    bool isSignalRoutable() const
    {
        return net_class != NetClass::kSpecial
            && net_class != NetClass::kClock
            && !is_clock;
    }

    // Clock/special nets are intentionally skipped in current signal routing.
    bool isExcludedFromSignalRouting() const
    {
        return !isSignalRoutable();
    }
};

class RouterDB
{
public:
    void clear();

    void build(odb::dbDatabase* db,
               odb::dbBlock* block,
               const std::vector<std::string>& lef_files);

    int get3DNetCount() const;
    int get2DNetCount() const;

    EffectiveRC computeEffectiveRCForDie(DieId die) const;
    // Global-routing layer-aware proxy RC:
    // this estimates direction-preferred unit RC before detailed layer assignment.
    EffectiveRC computeDirectionalRCForDie(DieId die, bool horizontal_preferred) const;
    double computeEffectiveTopUnitRes() const;
    double computeEffectiveTopUnitCap() const;
    double computeEffectiveBottomUnitRes() const;
    double computeEffectiveBottomUnitCap() const;
    EffectiveRC getDefaultWireRCForNetClass(NetClass cls) const;
    EffectiveRC getSignalWireRCFallback() const;
    EffectiveRC getClockWireRCFallback() const;
    double getViaResistanceOrDefault(const std::string& via_name) const;
    double getHBTResistanceOrDefault() const;
    std::vector<std::string> getTopGuideCandidateLayers() const;
    std::vector<std::string> getBottomGuideCandidateLayers() const;
    std::vector<const Net*> getRouteableNets() const;
    const std::vector<Pin>* getPins(const std::string& net_name) const;
    const std::vector<PinShape>* getPinShapes(const std::string& net_name,
                                              const std::string& pin_name) const;
    const LayerInfo* getRoutingLayer(const std::string& layer_name) const;
    std::string getLayerDirection(const std::string& layer_name) const;
    int getLayerLevel(const std::string& layer_name) const;
    std::vector<std::string> getAdjacentLayers(const std::string& layer_name) const;
    bool hasTrackInBox(const std::string& layer_name, const Box2D& box) const;
    NetClass classifyNet(const Net& net) const;
    std::string getPreferredHorizontalLayer(DieId die) const;
    std::string getPreferredVerticalLayer(DieId die) const;
    bool isKnownRoutingLayer(const std::string& layer_name) const;
    bool isHBLayer(const std::string& layer_name) const;

    // Convert a geometric length in DBU to micron-equivalent length used by
    // current RC proxy model. Falls back to raw DBU value if dbu_per_micron is unavailable.
    double dbuToMicronLength(int length_dbu) const;

public:
    int die_lx = 0;
    int die_ly = 0;
    int die_ux = 0;
    int die_uy = 0;

    int dbu_per_micron = 0;

    std::vector<LayerInfo> routing_layers;
    HBTInfo hbt;

    std::vector<Node> nodes;
    std::vector<Net> nets;
    std::unordered_map<std::string, int> node_name_to_id;

private:
    void buildDieArea(odb::dbBlock* block);
    void buildRoutingLayers(odb::dbTech* tech,
                            odb::dbBlock* block,
                            const std::vector<std::string>& lef_files);
    void buildNodes(odb::dbBlock* block);
    void buildNets(odb::dbBlock* block);
    DieId inferNetDominantDieFor2D(const Net& net) const;
    void finalizeNetPinDies(Net& net) const;
    void extractHBTInfo(odb::dbDatabase* db,
                        const std::vector<std::string>& lef_files);

    bool extractLayerRCFromLefText(const std::vector<std::string>& lef_files);
    void applyLayerRCFallbackDefaults();
    bool extractViaResistanceFromLefText(const std::vector<std::string>& lef_files);

    DieId inferDieFromMasterOrInst(const std::string& master_name,
                                   const std::string& inst_name) const;

    Point2D getITermPinLocation(odb::dbITerm* iterm) const;
    Point2D getBTermPinLocation(odb::dbBTerm* bterm) const;
    Box2D getITermPinBBox(odb::dbITerm* iterm) const;
    Box2D getBTermPinBBox(odb::dbBTerm* bterm) const;
    std::string inferITermPinLayerName(odb::dbMTerm* mterm) const;
    std::vector<PinShape> collectITermPinShapes(odb::dbITerm* iterm) const;
    std::vector<PinShape> collectBTermPinShapes(odb::dbBTerm* bterm) const;
    void refreshNetClass(Net& net) const;

private:
    std::unordered_map<std::string, double> via_resistance_from_db_;
    bool has_hb_layer_resistance_from_db_ = false;
    double hb_layer_resistance_from_db_ = 0.0;
    std::unordered_map<std::string, int> net_name_to_id_;
};
