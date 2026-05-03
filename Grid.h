#pragma once

#include "RouterDB.h"

#include <memory>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

// ============================================================
// 1. 基础枚举与结构
// ============================================================

enum class GridDomain
{
    kTop = 0,
    kHBT = 1,
    kBottom = 2
};

enum class GridEdgeType
{
    kTopWire = 0,
    kBottomWire = 1,
    kTopToHBT = 2,
    kBottomToHBT = 3
};

struct GridCoord
{
    int x = 0;
    int y = 0;
    int z = 0;  // 0: top, 1: hbt, 2: bottom
};

struct GridNode
{
    int id = -1;
    GridDomain domain = GridDomain::kTop;
    GridCoord coord;

    int center_x = 0;  // DBU
    int center_y = 0;  // DBU

    int capacity = 0;
    int demand = 0;
    bool blocked = false;

    // 若该节点映射到 HBT site，则 object_id = site_id；否则为 -1
    int object_id = -1;
};

struct GridEdge
{
    int id = -1;
    int u = -1;
    int v = -1;

    GridEdgeType type = GridEdgeType::kTopWire;

    int capacity = 0;
    int demand = 0;
    bool enabled = true;

    // 搜索基础代价（几何长度 + 接入惩罚）
    double cost = 0.0;

    // 用于时序驱动的等效 RC
    double fixed_res = 0.0;
    double fixed_cap = 0.0;
};

struct HBTSite
{
    int id = -1;
    int x = 0;           // DBU
    int y = 0;           // DBU
    int node_id = -1;    // HBT 层对应图节点

    int top_node_id = -1;
    int bottom_node_id = -1;

    bool occupied = false;
    int owner_net_id = -1;
};

// ============================================================
// 2. 构建配置
// ============================================================

struct GridBuildOptions
{
    // 若 <= 0，则自动使用 HBT pitch 作为 gcell 尺寸；若 HBT pitch 也不可用，则使用 die_size/32
    int top_gcell_width = -1;
    int top_gcell_height = -1;
    int bottom_gcell_width = -1;
    int bottom_gcell_height = -1;

    // 单层 wire edge 默认容量
    int top_wire_capacity = 10;
    int bottom_wire_capacity = 10;

    // 单位长度 RC（DBU 尺度）
    // 若当前 RouterDB 尚未存储 layer RC，可通过这里外部覆盖
    double top_unit_res = 0.0;
    double top_unit_cap = 0.0;
    double bottom_unit_res = 0.0;
    double bottom_unit_cap = 0.0;

    // HBT site 容量与寄生
    int hbt_site_capacity = 1;
    double hbt_res = 0.0;
    double hbt_cap = 0.0;
    double hbt_access_penalty = 0.0;

    // 是否根据 fixed instance 粗略标记 blocked cell
    bool mark_fixed_inst_blockage = false;
};

// 用于近似 Elmore 计算：
// path_edge_ids[i] 是 source -> sink 路径上的第 i 条边；
// downstream_caps[i] 是穿过该边后该边下游的总电容（包含 sink pin cap / 子树电容 / 边电容等）。
// 该接口不替你做树分解，只负责按你给定的“路径边 + 下游电容”求 Elmore。
struct PathElmoreInput
{
    std::vector<int> path_edge_ids;
    std::vector<double> downstream_caps;
    double driver_res = 0.0;
};

// ============================================================
// 3. 顶层 GridGraph
// ============================================================

class GridGraph
{
public:
    GridGraph() = default;

    bool buildFromRouterDB(const RouterDB& db, const GridBuildOptions& options);
    void clear();

    // ------------------ 基础访问 ------------------
    int nodeCount() const { return static_cast<int>(nodes_.size()); }
    int edgeCount() const { return static_cast<int>(edges_.size()); }

    const GridNode& node(int node_id) const { return nodes_.at(node_id); }
    GridNode& node(int node_id) { return nodes_.at(node_id); }

    const GridEdge& edge(int edge_id) const { return edges_.at(edge_id); }
    GridEdge& edge(int edge_id) { return edges_.at(edge_id); }

    const std::vector<int>& adjacency(int node_id) const { return adj_.at(node_id); }

    int otherEndpoint(int edge_id, int node_id) const;

    // ------------------ 资源接口 ------------------
    int edgeOverflow(int edge_id) const;
    void addDemandToEdge(int edge_id, int amount = 1);
    void removeDemandFromEdge(int edge_id, int amount = 1);

    bool reserveHBTSite(int site_id, int net_id);
    void releaseHBTSite(int site_id);

    // ------------------ 坐标映射 ------------------
    bool coordToTopGCell(int x, int y, int& gx, int& gy) const;
    bool coordToBottomGCell(int x, int y, int& gx, int& gy) const;

    int topNodeId(int gx, int gy) const;
    int bottomNodeId(int gx, int gy) const;

    // 将 pin 映射到 grid node。
    // 规则：
    // 1) top/bottom pin -> 对应 die 的 gcell node
    // 2) unknown die pin -> 默认映射到 top（通常是 bterm，可后续按 net 再修）
    int mapPinToGridNode(const Pin& pin) const;

    // 找到距离 (x,y) 最近的前 k 个可用 HBT site（按曼哈顿距离排序）
    std::vector<int> queryNearestAvailableHBTSites(int x, int y, int k) const;

    // ------------------ RC / Elmore 接口 ------------------
    double edgeResistance(int edge_id) const { return edges_.at(edge_id).fixed_res; }
    double edgeCapacitance(int edge_id) const { return edges_.at(edge_id).fixed_cap; }

    double computePathResistance(const std::vector<int>& edge_ids) const;
    double computePathCapacitance(const std::vector<int>& edge_ids) const;

    // 严格按输入的 downstream_caps 计算路径 Elmore
    double computePathElmoreDelay(const PathElmoreInput& input) const;

    // 简化版本：仅按路径自身的 RC 做近似，不考虑分支子树
    double computeSimplePathDelay(const std::vector<int>& edge_ids,
                                  double driver_res,
                                  double sink_cap) const;

    // ------------------ 调试输出 ------------------
    void printSummary() const;

    // ------------------ 公共配置读取 ------------------
    int dbuPerMicron() const { return dbu_per_micron_; }
    int dieLX() const { return die_lx_; }
    int dieLY() const { return die_ly_; }
    int dieUX() const { return die_ux_; }
    int dieUY() const { return die_uy_; }

    int topNumX() const { return top_num_x_; }
    int topNumY() const { return top_num_y_; }
    int bottomNumX() const { return bottom_num_x_; }
    int bottomNumY() const { return bottom_num_y_; }

    int topGCellWidth() const { return top_gcell_w_; }
    int topGCellHeight() const { return top_gcell_h_; }
    int bottomGCellWidth() const { return bottom_gcell_w_; }
    int bottomGCellHeight() const { return bottom_gcell_h_; }

    const std::vector<HBTSite>& hbtSites() const { return hbt_sites_; }
    std::vector<HBTSite>& hbtSites() { return hbt_sites_; }

private:
    int addNode(const GridNode& node);
    int addUndirectedEdge(const GridEdge& edge);

    void buildTopGrid(const RouterDB& db, const GridBuildOptions& options);
    void buildBottomGrid(const RouterDB& db, const GridBuildOptions& options);
    void buildHBTGrid(const RouterDB& db, const GridBuildOptions& options);
    void connectHBTToDies(const GridBuildOptions& options);
    void markFixedInstBlockage(const RouterDB& db);

    static int ceilDiv(int a, int b);
    static int manhattan(int x1, int y1, int x2, int y2);
    static double safeAverage(double a, double b);

private:
    int dbu_per_micron_ = 0;
    int die_lx_ = 0;
    int die_ly_ = 0;
    int die_ux_ = 0;
    int die_uy_ = 0;

    int top_gcell_w_ = 0;
    int top_gcell_h_ = 0;
    int bottom_gcell_w_ = 0;
    int bottom_gcell_h_ = 0;

    int top_num_x_ = 0;
    int top_num_y_ = 0;
    int bottom_num_x_ = 0;
    int bottom_num_y_ = 0;

    // [gx][gy] -> node_id
    std::vector<std::vector<int>> top_node_ids_;
    std::vector<std::vector<int>> bottom_node_ids_;

    std::vector<GridNode> nodes_;
    std::vector<GridEdge> edges_;
    std::vector<std::vector<int>> adj_;

    std::vector<HBTSite> hbt_sites_;
};

// ============================================================
// 4. F2F-HB 异构网格（Top / Bottom / HBT / Hybrid）
// ============================================================
//
// 说明：
// - 该部分是面向 F2F Hybrid Bonding 场景新增的“异构网格”实现。
// - Top/Bottom 使用独立 routing grid；HBT 使用独立资源阵列。
// - 当前 RouterDB 中 routing_layers / obstacle 尚未完整区分 die，因此这里保留了
//   可扩展接口并采用工程临时策略：
//   1) Top 与 Bottom 暂时共用相同 layer 统计逻辑（后续可分离）。
//   2) obstacle 过滤接口已预留；当 RouterDB 补充 obstacle.die 后可直接收敛到每个 die。

struct GridCell
{
    int ix = -1;
    int iy = -1;
    int lx = 0;
    int ly = 0;
    int ux = 0;
    int uy = 0;

    std::vector<int> incident_hbts;
    std::vector<int> pin_ids;
};

struct HBTSlot
{
    int id = -1;
    int x = 0;
    int y = 0;
    bool occupied = false;
};

struct HBTBinding
{
    int hbt_id = -1;
    int top_ix = -1;
    int top_iy = -1;
    int bottom_ix = -1;
    int bottom_iy = -1;
};

// RouterDB 当前未公开 obstacle 列表，这里先定义可扩展的抽象矩形结构。
struct ObstacleRect
{
    int lx = 0;
    int ly = 0;
    int ux = 0;
    int uy = 0;
};

class RoutingGridBase
{
public:
    virtual ~RoutingGridBase() = default;

    void build(const RouterDB& db, const std::vector<Point2D>& hbt_points);
    int locateX(int x) const;
    int locateY(int y) const;
    int locateCell(int x, int y, int& ix, int& iy) const;
    void refineAroundPinsAndObstacles(const RouterDB& db);
    void buildCells();

    const std::vector<int>& xLines() const { return x_lines_; }
    const std::vector<int>& yLines() const { return y_lines_; }
    const std::vector<GridCell>& cells() const { return cells_; }
    std::vector<GridCell>& cells() { return cells_; }

    const GridCell* getCell(int ix, int iy) const;
    GridCell* getCell(int ix, int iy);

protected:
    virtual bool acceptPin(const Pin& pin) const = 0;
    virtual const char* gridName() const = 0;

private:
    void collectSeedLines(const RouterDB& db, const std::vector<Point2D>& hbt_points);
    void addBasePitchFillLines(const RouterDB& db);
    int chooseRepresentativePitch(const RouterDB& db) const;
    void dedupAndClampLines(const RouterDB& db);
    void attachPinsToCells(const RouterDB& db);
    std::vector<ObstacleRect> collectObstaclesForCurrentDie(const RouterDB& db) const;

protected:
    int die_lx_ = 0;
    int die_ly_ = 0;
    int die_ux_ = 0;
    int die_uy_ = 0;

    std::vector<int> x_lines_;
    std::vector<int> y_lines_;
    std::vector<GridCell> cells_;
    std::vector<std::vector<int>> cell_index_;  // [ix][iy] -> cell id
};

class TopRoutingGrid : public RoutingGridBase
{
public:
    void build(const RouterDB& db, const std::vector<Point2D>& hbt_points = {});
    bool acceptPin(const Pin& pin) const override;
    const char* gridName() const override { return "TopRoutingGrid"; }
};

class BottomRoutingGrid : public RoutingGridBase
{
public:
    void build(const RouterDB& db, const std::vector<Point2D>& hbt_points = {});
    bool acceptPin(const Pin& pin) const override;
    const char* gridName() const override { return "BottomRoutingGrid"; }
};

class HBTGrid
{
public:
    void build(const RouterDB& db, int origin_x, int origin_y);
    const std::vector<HBTSlot>& getSlots() const { return slots_; }
    std::vector<HBTSlot>& getSlots() { return slots_; }
    std::vector<int> querySlotsInBox(int lx, int ly, int ux, int uy) const;
    std::vector<int> queryNearestSlots(int x, int y, int k) const;
    std::vector<int> queryNearestSlotsToBox(int lx, int ly, int ux, int uy, int k) const;

private:
    void rebuildSpatialBuckets();
    long long makeBucketKey(int bx, int by) const;

    std::vector<HBTSlot> slots_;
    int origin_x_ = 0;
    int origin_y_ = 0;
    int pitch_x_ = 1;
    int pitch_y_ = 1;
    std::unordered_map<long long, std::vector<int>> bucket_to_slots_;
};

class HybridGrid
{
public:
    void build(const RouterDB& db, int hbt_origin_x, int hbt_origin_y);
    void bindHBTToTopBottom();
    void validate(const RouterDB& db) const;

    TopRoutingGrid top;
    BottomRoutingGrid bottom;
    HBTGrid hbt;
    std::vector<HBTBinding> bindings;

private:
    const RouterDB* db_ = nullptr;
};
