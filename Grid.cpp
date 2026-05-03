#include "Grid.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

int GridGraph::ceilDiv(int a, int b)
{
    if (b <= 0) {
        throw std::invalid_argument("GridGraph::ceilDiv divisor must be positive");
    }
    return (a + b - 1) / b;
}

int GridGraph::manhattan(int x1, int y1, int x2, int y2)
{
    return std::abs(x1 - x2) + std::abs(y1 - y2);
}

double GridGraph::safeAverage(double a, double b)
{
    return 0.5 * (a + b);
}

void GridGraph::clear()
{
    dbu_per_micron_ = 0;
    die_lx_ = die_ly_ = die_ux_ = die_uy_ = 0;

    top_gcell_w_ = top_gcell_h_ = 0;
    bottom_gcell_w_ = bottom_gcell_h_ = 0;
    top_num_x_ = top_num_y_ = 0;
    bottom_num_x_ = bottom_num_y_ = 0;

    top_node_ids_.clear();
    bottom_node_ids_.clear();

    nodes_.clear();
    edges_.clear();
    adj_.clear();
    hbt_sites_.clear();
}

int GridGraph::addNode(const GridNode& input)
{
    GridNode node = input;
    node.id = static_cast<int>(nodes_.size());
    nodes_.push_back(node);
    adj_.emplace_back();
    return node.id;
}

int GridGraph::addUndirectedEdge(const GridEdge& input)
{
    if (input.u < 0 || input.u >= static_cast<int>(nodes_.size()) ||
        input.v < 0 || input.v >= static_cast<int>(nodes_.size())) {
        throw std::runtime_error("GridGraph::addUndirectedEdge invalid endpoint");
    }

    GridEdge edge = input;
    edge.id = static_cast<int>(edges_.size());
    edges_.push_back(edge);
    adj_[edge.u].push_back(edge.id);
    adj_[edge.v].push_back(edge.id);
    return edge.id;
}

bool GridGraph::buildFromRouterDB(const RouterDB& db, const GridBuildOptions& options)
{
    clear();

    dbu_per_micron_ = db.dbu_per_micron;
    die_lx_ = db.die_lx;
    die_ly_ = db.die_ly;
    die_ux_ = db.die_ux;
    die_uy_ = db.die_uy;

    buildTopGrid(db, options);
    buildBottomGrid(db, options);
    buildHBTGrid(db, options);
    connectHBTToDies(options);

    if (options.mark_fixed_inst_blockage) {
        markFixedInstBlockage(db);
    }

    return true;
}

void GridGraph::buildTopGrid(const RouterDB& db, const GridBuildOptions& options)
{
    const int die_w = std::max(1, die_ux_ - die_lx_);
    const int die_h = std::max(1, die_uy_ - die_ly_);

    int auto_pitch_x = db.hbt.pitch_x > 0 ? db.hbt.pitch_x : std::max(1, die_w / 32);
    int auto_pitch_y = db.hbt.pitch_y > 0 ? db.hbt.pitch_y : std::max(1, die_h / 32);

    top_gcell_w_ = (options.top_gcell_width > 0) ? options.top_gcell_width : auto_pitch_x;
    top_gcell_h_ = (options.top_gcell_height > 0) ? options.top_gcell_height : auto_pitch_y;

    top_num_x_ = std::max(1, ceilDiv(die_w, top_gcell_w_));
    top_num_y_ = std::max(1, ceilDiv(die_h, top_gcell_h_));
    top_node_ids_.assign(top_num_x_, std::vector<int>(top_num_y_, -1));

    for (int gx = 0; gx < top_num_x_; ++gx) {
        for (int gy = 0; gy < top_num_y_; ++gy) {
            GridNode node;
            node.domain = GridDomain::kTop;
            node.coord = {gx, gy, 0};
            node.center_x = die_lx_ + gx * top_gcell_w_ + top_gcell_w_ / 2;
            node.center_y = die_ly_ + gy * top_gcell_h_ + top_gcell_h_ / 2;
            node.capacity = options.top_wire_capacity;
            const int id = addNode(node);
            top_node_ids_[gx][gy] = id;
        }
    }

    for (int gx = 0; gx < top_num_x_; ++gx) {
        for (int gy = 0; gy < top_num_y_; ++gy) {
            const int u = top_node_ids_[gx][gy];

            if (gx + 1 < top_num_x_) {
                GridEdge e;
                e.u = u;
                e.v = top_node_ids_[gx + 1][gy];
                e.type = GridEdgeType::kTopWire;
                e.capacity = options.top_wire_capacity;
                e.cost = static_cast<double>(top_gcell_w_);
                e.fixed_res = options.top_unit_res * static_cast<double>(top_gcell_w_);
                e.fixed_cap = options.top_unit_cap * static_cast<double>(top_gcell_w_);
                addUndirectedEdge(e);
            }

            if (gy + 1 < top_num_y_) {
                GridEdge e;
                e.u = u;
                e.v = top_node_ids_[gx][gy + 1];
                e.type = GridEdgeType::kTopWire;
                e.capacity = options.top_wire_capacity;
                e.cost = static_cast<double>(top_gcell_h_);
                e.fixed_res = options.top_unit_res * static_cast<double>(top_gcell_h_);
                e.fixed_cap = options.top_unit_cap * static_cast<double>(top_gcell_h_);
                addUndirectedEdge(e);
            }
        }
    }
}

void GridGraph::buildBottomGrid(const RouterDB& db, const GridBuildOptions& options)
{
    const int die_w = std::max(1, die_ux_ - die_lx_);
    const int die_h = std::max(1, die_uy_ - die_ly_);

    int auto_pitch_x = db.hbt.pitch_x > 0 ? db.hbt.pitch_x : std::max(1, die_w / 32);
    int auto_pitch_y = db.hbt.pitch_y > 0 ? db.hbt.pitch_y : std::max(1, die_h / 32);

    bottom_gcell_w_ = (options.bottom_gcell_width > 0) ? options.bottom_gcell_width : auto_pitch_x;
    bottom_gcell_h_ = (options.bottom_gcell_height > 0) ? options.bottom_gcell_height : auto_pitch_y;

    bottom_num_x_ = std::max(1, ceilDiv(die_w, bottom_gcell_w_));
    bottom_num_y_ = std::max(1, ceilDiv(die_h, bottom_gcell_h_));
    bottom_node_ids_.assign(bottom_num_x_, std::vector<int>(bottom_num_y_, -1));

    for (int gx = 0; gx < bottom_num_x_; ++gx) {
        for (int gy = 0; gy < bottom_num_y_; ++gy) {
            GridNode node;
            node.domain = GridDomain::kBottom;
            node.coord = {gx, gy, 2};
            node.center_x = die_lx_ + gx * bottom_gcell_w_ + bottom_gcell_w_ / 2;
            node.center_y = die_ly_ + gy * bottom_gcell_h_ + bottom_gcell_h_ / 2;
            node.capacity = options.bottom_wire_capacity;
            const int id = addNode(node);
            bottom_node_ids_[gx][gy] = id;
        }
    }

    for (int gx = 0; gx < bottom_num_x_; ++gx) {
        for (int gy = 0; gy < bottom_num_y_; ++gy) {
            const int u = bottom_node_ids_[gx][gy];

            if (gx + 1 < bottom_num_x_) {
                GridEdge e;
                e.u = u;
                e.v = bottom_node_ids_[gx + 1][gy];
                e.type = GridEdgeType::kBottomWire;
                e.capacity = options.bottom_wire_capacity;
                e.cost = static_cast<double>(bottom_gcell_w_);
                e.fixed_res = options.bottom_unit_res * static_cast<double>(bottom_gcell_w_);
                e.fixed_cap = options.bottom_unit_cap * static_cast<double>(bottom_gcell_w_);
                addUndirectedEdge(e);
            }

            if (gy + 1 < bottom_num_y_) {
                GridEdge e;
                e.u = u;
                e.v = bottom_node_ids_[gx][gy + 1];
                e.type = GridEdgeType::kBottomWire;
                e.capacity = options.bottom_wire_capacity;
                e.cost = static_cast<double>(bottom_gcell_h_);
                e.fixed_res = options.bottom_unit_res * static_cast<double>(bottom_gcell_h_);
                e.fixed_cap = options.bottom_unit_cap * static_cast<double>(bottom_gcell_h_);
                addUndirectedEdge(e);
            }
        }
    }
}

void GridGraph::buildHBTGrid(const RouterDB& db, const GridBuildOptions& options)
{
    int pitch_x = db.hbt.pitch_x;
    int pitch_y = db.hbt.pitch_y;
    if (pitch_x <= 0 || pitch_y <= 0) {
        // fallback: 若 RouterDB 尚未抽出 HBT pitch，则退化为较粗网格
        pitch_x = std::max(1, (die_ux_ - die_lx_) / 16);
        pitch_y = std::max(1, (die_uy_ - die_ly_) / 16);
    }

    const int start_x = die_lx_;
    const int start_y = die_ly_;

    int site_id = 0;
    for (int y = start_y; y < die_uy_; y += pitch_y) {
        for (int x = start_x; x < die_ux_; x += pitch_x) {
            HBTSite site;
            site.id = site_id++;
            site.x = x;
            site.y = y;

            GridNode node;
            node.domain = GridDomain::kHBT;
            node.coord = {site.id, 0, 1};
            node.center_x = x;
            node.center_y = y;
            node.capacity = options.hbt_site_capacity;
            node.object_id = site.id;

            site.node_id = addNode(node);
            hbt_sites_.push_back(site);
        }
    }
}

void GridGraph::connectHBTToDies(const GridBuildOptions& options)
{
    for (auto& site : hbt_sites_) {
        int top_gx = -1, top_gy = -1;
        int bot_gx = -1, bot_gy = -1;

        if (!coordToTopGCell(site.x, site.y, top_gx, top_gy)) {
            continue;
        }
        if (!coordToBottomGCell(site.x, site.y, bot_gx, bot_gy)) {
            continue;
        }

        const int top_node_id = topNodeId(top_gx, top_gy);
        const int bot_node_id = bottomNodeId(bot_gx, bot_gy);
        if (top_node_id < 0 || bot_node_id < 0) {
            continue;
        }

        site.top_node_id = top_node_id;
        site.bottom_node_id = bot_node_id;

        const GridNode& top_node = node(top_node_id);
        const GridNode& bot_node = node(bot_node_id);

        {
            GridEdge e;
            e.u = top_node_id;
            e.v = site.node_id;
            e.type = GridEdgeType::kTopToHBT;
            e.capacity = options.hbt_site_capacity;
            e.cost = static_cast<double>(manhattan(top_node.center_x, top_node.center_y,
                                                   site.x, site.y))
                   + options.hbt_access_penalty;
            e.fixed_res = 0.5 * options.hbt_res;
            e.fixed_cap = 0.5 * options.hbt_cap;
            addUndirectedEdge(e);
        }

        {
            GridEdge e;
            e.u = bot_node_id;
            e.v = site.node_id;
            e.type = GridEdgeType::kBottomToHBT;
            e.capacity = options.hbt_site_capacity;
            e.cost = static_cast<double>(manhattan(bot_node.center_x, bot_node.center_y,
                                                   site.x, site.y))
                   + options.hbt_access_penalty;
            e.fixed_res = 0.5 * options.hbt_res;
            e.fixed_cap = 0.5 * options.hbt_cap;
            addUndirectedEdge(e);
        }
    }
}

void GridGraph::markFixedInstBlockage(const RouterDB& db)
{
    // 当前版本仅做非常粗粒度的 fixed instance blockage：
    // 若一个 fixed inst 的中心落入某个 gcell，则把该 gcell node 标成 blocked。
    for (const auto& inst : db.nodes) {
        if (!inst.fixed) {
            continue;
        }

        int gx = -1, gy = -1;
        if (inst.die == DieId::kTop) {
            if (coordToTopGCell(inst.x + inst.width / 2, inst.y + inst.height / 2, gx, gy)) {
                const int node_id = topNodeId(gx, gy);
                if (node_id >= 0) {
                    nodes_[node_id].blocked = true;
                }
            }
        } else if (inst.die == DieId::kBottom) {
            if (coordToBottomGCell(inst.x + inst.width / 2, inst.y + inst.height / 2, gx, gy)) {
                const int node_id = bottomNodeId(gx, gy);
                if (node_id >= 0) {
                    nodes_[node_id].blocked = true;
                }
            }
        }
    }
}

int GridGraph::otherEndpoint(int edge_id, int node_id) const
{
    const auto& e = edges_.at(edge_id);
    if (e.u == node_id) {
        return e.v;
    }
    if (e.v == node_id) {
        return e.u;
    }
    return -1;
}

int GridGraph::edgeOverflow(int edge_id) const
{
    const auto& e = edges_.at(edge_id);
    return std::max(0, e.demand - e.capacity);
}

void GridGraph::addDemandToEdge(int edge_id, int amount)
{
    edges_.at(edge_id).demand += amount;
}

void GridGraph::removeDemandFromEdge(int edge_id, int amount)
{
    auto& e = edges_.at(edge_id);
    e.demand = std::max(0, e.demand - amount);
}

bool GridGraph::reserveHBTSite(int site_id, int net_id)
{
    if (site_id < 0 || site_id >= static_cast<int>(hbt_sites_.size())) {
        return false;
    }

    auto& site = hbt_sites_[site_id];
    if (site.occupied) {
        return false;
    }

    site.occupied = true;
    site.owner_net_id = net_id;

    auto& hbt_node = nodes_.at(site.node_id);
    hbt_node.blocked = true;

    // 禁用与该 HBT 连接的接入边，避免其他 net 再使用
    for (int edge_id : adj_.at(site.node_id)) {
        edges_.at(edge_id).enabled = false;
    }

    return true;
}

void GridGraph::releaseHBTSite(int site_id)
{
    if (site_id < 0 || site_id >= static_cast<int>(hbt_sites_.size())) {
        return;
    }

    auto& site = hbt_sites_[site_id];
    site.occupied = false;
    site.owner_net_id = -1;

    auto& hbt_node = nodes_.at(site.node_id);
    hbt_node.blocked = false;

    for (int edge_id : adj_.at(site.node_id)) {
        edges_.at(edge_id).enabled = true;
    }
}

bool GridGraph::coordToTopGCell(int x, int y, int& gx, int& gy) const
{
    if (x < die_lx_ || x >= die_ux_ || y < die_ly_ || y >= die_uy_) {
        return false;
    }
    gx = (x - die_lx_) / top_gcell_w_;
    gy = (y - die_ly_) / top_gcell_h_;
    return gx >= 0 && gx < top_num_x_ && gy >= 0 && gy < top_num_y_;
}

bool GridGraph::coordToBottomGCell(int x, int y, int& gx, int& gy) const
{
    if (x < die_lx_ || x >= die_ux_ || y < die_ly_ || y >= die_uy_) {
        return false;
    }
    gx = (x - die_lx_) / bottom_gcell_w_;
    gy = (y - die_ly_) / bottom_gcell_h_;
    return gx >= 0 && gx < bottom_num_x_ && gy >= 0 && gy < bottom_num_y_;
}

int GridGraph::topNodeId(int gx, int gy) const
{
    if (gx < 0 || gx >= top_num_x_ || gy < 0 || gy >= top_num_y_) {
        return -1;
    }
    return top_node_ids_[gx][gy];
}

int GridGraph::bottomNodeId(int gx, int gy) const
{
    if (gx < 0 || gx >= bottom_num_x_ || gy < 0 || gy >= bottom_num_y_) {
        return -1;
    }
    return bottom_node_ids_[gx][gy];
}

int GridGraph::mapPinToGridNode(const Pin& pin) const
{
    int gx = -1;
    int gy = -1;

    if (pin.die == DieId::kTop) {
        if (coordToTopGCell(pin.x, pin.y, gx, gy)) {
            return topNodeId(gx, gy);
        }
    } else if (pin.die == DieId::kBottom) {
        if (coordToBottomGCell(pin.x, pin.y, gx, gy)) {
            return bottomNodeId(gx, gy);
        }
    } else {
        // unknown：这里保持 legacy fallback 到 top。
        // NOTE:
        //   RouterDB/PDTreeRouter 已在 2D net 内优先补全/解析 Unknown pin die，
        //   2D attach 不再依赖这里的粗粒度 Unknown->Top 假设。
        if (coordToTopGCell(pin.x, pin.y, gx, gy)) {
            return topNodeId(gx, gy);
        }
    }

    return -1;
}

std::vector<int> GridGraph::queryNearestAvailableHBTSites(int x, int y, int k) const
{
    struct Candidate {
        int site_id;
        int dist;
    };

    std::vector<Candidate> cands;
    cands.reserve(hbt_sites_.size());

    for (const auto& site : hbt_sites_) {
        if (site.occupied) {
            continue;
        }
        cands.push_back({site.id, manhattan(x, y, site.x, site.y)});
    }

    std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
        if (a.dist != b.dist) {
            return a.dist < b.dist;
        }
        return a.site_id < b.site_id;
    });

    std::vector<int> result;
    for (int i = 0; i < static_cast<int>(cands.size()) && i < k; ++i) {
        result.push_back(cands[i].site_id);
    }
    return result;
}

double GridGraph::computePathResistance(const std::vector<int>& edge_ids) const
{
    double r = 0.0;
    for (int edge_id : edge_ids) {
        r += edges_.at(edge_id).fixed_res;
    }
    return r;
}

double GridGraph::computePathCapacitance(const std::vector<int>& edge_ids) const
{
    double c = 0.0;
    for (int edge_id : edge_ids) {
        c += edges_.at(edge_id).fixed_cap;
    }
    return c;
}

double GridGraph::computePathElmoreDelay(const PathElmoreInput& input) const
{
    if (input.path_edge_ids.size() != input.downstream_caps.size()) {
        throw std::runtime_error("computePathElmoreDelay: size mismatch between path_edge_ids and downstream_caps");
    }

    double delay = 0.0;
    if (!input.downstream_caps.empty()) {
        delay += input.driver_res * input.downstream_caps.front();
    }

    for (size_t i = 0; i < input.path_edge_ids.size(); ++i) {
        const GridEdge& e = edges_.at(input.path_edge_ids[i]);
        delay += e.fixed_res * input.downstream_caps[i];
    }

    return delay;
}

double GridGraph::computeSimplePathDelay(const std::vector<int>& edge_ids,
                                         double driver_res,
                                         double sink_cap) const
{
    // 仅按“路径本身 + sink_cap”做近似：
    // downstream_cap_i = sink_cap + path 上位于该边之后(含自身)的全部 edge cap
    std::vector<double> downstream(edge_ids.size(), 0.0);
    double suffix_cap = sink_cap;

    for (int i = static_cast<int>(edge_ids.size()) - 1; i >= 0; --i) {
        suffix_cap += edges_.at(edge_ids[i]).fixed_cap;
        downstream[i] = suffix_cap;
    }

    PathElmoreInput input;
    input.path_edge_ids = edge_ids;
    input.downstream_caps = downstream;
    input.driver_res = driver_res;
    return computePathElmoreDelay(input);
}

void GridGraph::printSummary() const
{
    int top_nodes = 0;
    int bottom_nodes = 0;
    int hbt_nodes = 0;
    int top_wire_edges = 0;
    int bottom_wire_edges = 0;
    int hbt_access_edges = 0;

    for (const auto& n : nodes_) {
        if (n.domain == GridDomain::kTop) {
            ++top_nodes;
        } else if (n.domain == GridDomain::kBottom) {
            ++bottom_nodes;
        } else {
            ++hbt_nodes;
        }
    }

    for (const auto& e : edges_) {
        if (e.type == GridEdgeType::kTopWire) {
            ++top_wire_edges;
        } else if (e.type == GridEdgeType::kBottomWire) {
            ++bottom_wire_edges;
        } else {
            ++hbt_access_edges;
        }
    }

    std::cout << "========== GridGraph Summary ==========" << std::endl;
    std::cout << "die        : (" << die_lx_ << ", " << die_ly_ << ") - ("
              << die_ux_ << ", " << die_uy_ << ")" << std::endl;
    std::cout << "top gcell  : " << top_gcell_w_ << " x " << top_gcell_h_
              << ", dims = " << top_num_x_ << " x " << top_num_y_ << std::endl;
    std::cout << "bot gcell  : " << bottom_gcell_w_ << " x " << bottom_gcell_h_
              << ", dims = " << bottom_num_x_ << " x " << bottom_num_y_ << std::endl;
    std::cout << "nodes      : total=" << nodes_.size()
              << ", top=" << top_nodes
              << ", hbt=" << hbt_nodes
              << ", bottom=" << bottom_nodes << std::endl;
    std::cout << "edges      : total=" << edges_.size()
              << ", topWire=" << top_wire_edges
              << ", bottomWire=" << bottom_wire_edges
              << ", hbtAccess=" << hbt_access_edges << std::endl;
    std::cout << "hbt sites  : " << hbt_sites_.size() << std::endl;
    std::cout << "=======================================" << std::endl;
}

// ============================================================
// 4) F2F-HB 异构网格实现
// ============================================================

namespace {

constexpr int kInvalid = -1;

int clampToDie(int v, int lo, int hi)
{
    return std::max(lo, std::min(v, hi));
}

bool pointInsideHalfOpenCell(const GridCell& c, int x, int y)
{
    return (x >= c.lx && x < c.ux && y >= c.ly && y < c.uy);
}

bool pointInsideDieInclusive(const RouterDB& db, int x, int y)
{
    return (x >= db.die_lx && x <= db.die_ux && y >= db.die_ly && y <= db.die_uy);
}

}  // namespace

void RoutingGridBase::build(const RouterDB& db, const std::vector<Point2D>& hbt_points)
{
    die_lx_ = db.die_lx;
    die_ly_ = db.die_ly;
    die_ux_ = db.die_ux;
    die_uy_ = db.die_uy;

    x_lines_.clear();
    y_lines_.clear();
    cells_.clear();
    cell_index_.clear();

    collectSeedLines(db, hbt_points);
    addBasePitchFillLines(db);
    dedupAndClampLines(db);
    buildCells();
    refineAroundPinsAndObstacles(db);
}

int RoutingGridBase::locateX(int x) const
{
    if (x_lines_.size() < 2) {
        return kInvalid;
    }
    if (x < x_lines_.front() || x > x_lines_.back()) {
        return kInvalid;
    }
    if (x == x_lines_.back()) {
        return static_cast<int>(x_lines_.size()) - 2;
    }

    const auto it = std::upper_bound(x_lines_.begin(), x_lines_.end(), x);
    const int ix = static_cast<int>(std::distance(x_lines_.begin(), it)) - 1;
    return (ix >= 0 && ix < static_cast<int>(x_lines_.size()) - 1) ? ix : kInvalid;
}

int RoutingGridBase::locateY(int y) const
{
    if (y_lines_.size() < 2) {
        return kInvalid;
    }
    if (y < y_lines_.front() || y > y_lines_.back()) {
        return kInvalid;
    }
    if (y == y_lines_.back()) {
        return static_cast<int>(y_lines_.size()) - 2;
    }

    const auto it = std::upper_bound(y_lines_.begin(), y_lines_.end(), y);
    const int iy = static_cast<int>(std::distance(y_lines_.begin(), it)) - 1;
    return (iy >= 0 && iy < static_cast<int>(y_lines_.size()) - 1) ? iy : kInvalid;
}

int RoutingGridBase::locateCell(int x, int y, int& ix, int& iy) const
{
    ix = locateX(x);
    iy = locateY(y);
    if (ix < 0 || iy < 0 || ix >= static_cast<int>(cell_index_.size()) ||
        iy >= static_cast<int>(cell_index_[ix].size())) {
        return kInvalid;
    }

    const int cid = cell_index_[ix][iy];
    if (cid < 0 || cid >= static_cast<int>(cells_.size())) {
        return kInvalid;
    }

    const GridCell& c = cells_[cid];
    if (!pointInsideHalfOpenCell(c, x, y)) {
        return kInvalid;
    }
    return cid;
}

void RoutingGridBase::refineAroundPinsAndObstacles(const RouterDB& db)
{
    // 插入 pin 附近局部 refinement 线（当前版本仅 1 个 delta，强调清晰可扩展）。
    const int ref_pitch = std::max(1, chooseRepresentativePitch(db) / 2);

    for (const auto& net : db.nets) {
        for (const auto& pin : net.pins) {
            if (!acceptPin(pin)) {
                continue;
            }
            const int px = clampToDie(pin.x, die_lx_, die_ux_);
            const int py = clampToDie(pin.y, die_ly_, die_uy_);

            x_lines_.push_back(px);
            y_lines_.push_back(py);
            x_lines_.push_back(clampToDie(px - ref_pitch, die_lx_, die_ux_));
            x_lines_.push_back(clampToDie(px + ref_pitch, die_lx_, die_ux_));
            y_lines_.push_back(clampToDie(py - ref_pitch, die_ly_, die_uy_));
            y_lines_.push_back(clampToDie(py + ref_pitch, die_ly_, die_uy_));
        }
    }

    // 注意：RouterDB 当前版本未公开 obstacle 容器，这里通过统一接口返回空列表。
    // 后续当 RouterDB 增加 obstacle + die 信息时，可在 collectObstaclesForCurrentDie
    // 中接入并启用分 die refine。
    const auto obstacles = collectObstaclesForCurrentDie(db);
    for (const auto& o : obstacles) {
        x_lines_.push_back(clampToDie(o.lx, die_lx_, die_ux_));
        x_lines_.push_back(clampToDie(o.ux, die_lx_, die_ux_));
        y_lines_.push_back(clampToDie(o.ly, die_ly_, die_uy_));
        y_lines_.push_back(clampToDie(o.uy, die_ly_, die_uy_));
    }

    dedupAndClampLines(db);
    buildCells();
    attachPinsToCells(db);
}

void RoutingGridBase::buildCells()
{
    cells_.clear();
    if (x_lines_.size() < 2 || y_lines_.size() < 2) {
        cell_index_.clear();
        return;
    }

    const int nx = static_cast<int>(x_lines_.size()) - 1;
    const int ny = static_cast<int>(y_lines_.size()) - 1;
    cell_index_.assign(nx, std::vector<int>(ny, kInvalid));

    for (int ix = 0; ix < nx; ++ix) {
        for (int iy = 0; iy < ny; ++iy) {
            GridCell c;
            c.ix = ix;
            c.iy = iy;
            c.lx = x_lines_[ix];
            c.ux = x_lines_[ix + 1];
            c.ly = y_lines_[iy];
            c.uy = y_lines_[iy + 1];

            // left-closed, right-open; 最后一列/行通过 locateX/locateY 对 == die_ux/uy 特判归并。
            const int cid = static_cast<int>(cells_.size());
            cells_.push_back(c);
            cell_index_[ix][iy] = cid;
        }
    }
}

const GridCell* RoutingGridBase::getCell(int ix, int iy) const
{
    if (ix < 0 || iy < 0 || ix >= static_cast<int>(cell_index_.size()) ||
        iy >= static_cast<int>(cell_index_[ix].size())) {
        return nullptr;
    }
    const int cid = cell_index_[ix][iy];
    return (cid >= 0 && cid < static_cast<int>(cells_.size())) ? &cells_[cid] : nullptr;
}

GridCell* RoutingGridBase::getCell(int ix, int iy)
{
    if (ix < 0 || iy < 0 || ix >= static_cast<int>(cell_index_.size()) ||
        iy >= static_cast<int>(cell_index_[ix].size())) {
        return nullptr;
    }
    const int cid = cell_index_[ix][iy];
    return (cid >= 0 && cid < static_cast<int>(cells_.size())) ? &cells_[cid] : nullptr;
}

void RoutingGridBase::collectSeedLines(const RouterDB& db, const std::vector<Point2D>& hbt_points)
{
    x_lines_.push_back(db.die_lx);
    x_lines_.push_back(db.die_ux);
    y_lines_.push_back(db.die_ly);
    y_lines_.push_back(db.die_uy);

    for (const auto& net : db.nets) {
        for (const auto& pin : net.pins) {
            if (!acceptPin(pin)) {
                continue;
            }
            x_lines_.push_back(clampToDie(pin.x, db.die_lx, db.die_ux));
            y_lines_.push_back(clampToDie(pin.y, db.die_ly, db.die_uy));
        }
    }

    for (const auto& p : hbt_points) {
        if (!pointInsideDieInclusive(db, p.x, p.y)) {
            continue;
        }
        x_lines_.push_back(p.x);
        y_lines_.push_back(p.y);
    }

    for (const auto& o : collectObstaclesForCurrentDie(db)) {
        x_lines_.push_back(clampToDie(o.lx, db.die_lx, db.die_ux));
        x_lines_.push_back(clampToDie(o.ux, db.die_lx, db.die_ux));
        y_lines_.push_back(clampToDie(o.ly, db.die_ly, db.die_uy));
        y_lines_.push_back(clampToDie(o.uy, db.die_ly, db.die_uy));
    }
}

void RoutingGridBase::addBasePitchFillLines(const RouterDB& db)
{
    const int pitch = chooseRepresentativePitch(db);
    if (pitch <= 0) {
        return;
    }

    auto fillOneAxis = [pitch](std::vector<int>& lines) {
        std::sort(lines.begin(), lines.end());
        std::vector<int> additional;
        for (size_t i = 0; i + 1 < lines.size(); ++i) {
            const int a = lines[i];
            const int b = lines[i + 1];
            if (b - a <= 2 * pitch) {
                continue;
            }
            for (int v = a + pitch; v < b; v += pitch) {
                additional.push_back(v);
            }
        }
        lines.insert(lines.end(), additional.begin(), additional.end());
    };

    fillOneAxis(x_lines_);
    fillOneAxis(y_lines_);
}

int RoutingGridBase::chooseRepresentativePitch(const RouterDB& db) const
{
    std::vector<int> candidates;
    candidates.reserve(db.routing_layers.size() * 2 + 2);

    // 临时策略：RouterDB 尚无 top/bottom 分离 layer 视图，先共用同一份 routing_layers。
    // 后续可在这里按 die 分组统计 pitch。
    for (const auto& l : db.routing_layers) {
        if (l.width > 0) {
            candidates.push_back(l.width * 4);  // 用 width 的倍率作为保守代表尺度
        }
    }

    if (db.hbt.pitch_x > 0) {
        candidates.push_back(db.hbt.pitch_x);
    }
    if (db.hbt.pitch_y > 0) {
        candidates.push_back(db.hbt.pitch_y);
    }

    if (candidates.empty()) {
        const int die_w = std::max(1, db.die_ux - db.die_lx);
        const int die_h = std::max(1, db.die_uy - db.die_ly);
        return std::max(1, std::min(die_w, die_h) / 32);
    }

    std::sort(candidates.begin(), candidates.end());
    return candidates[candidates.size() / 2];
}

void RoutingGridBase::dedupAndClampLines(const RouterDB& db)
{
    auto norm = [&db](std::vector<int>& lines, int lo, int hi) {
        for (int& v : lines) {
            v = clampToDie(v, lo, hi);
        }
        lines.push_back(lo);
        lines.push_back(hi);
        std::sort(lines.begin(), lines.end());
        lines.erase(std::unique(lines.begin(), lines.end()), lines.end());

        if (lines.size() < 2) {
            lines = {lo, hi};
        }
        if (lines.front() != lo) {
            lines.insert(lines.begin(), lo);
        }
        if (lines.back() != hi) {
            lines.push_back(hi);
        }
    };

    norm(x_lines_, db.die_lx, db.die_ux);
    norm(y_lines_, db.die_ly, db.die_uy);
}

void RoutingGridBase::attachPinsToCells(const RouterDB& db)
{
    int pin_id = 0;
    for (const auto& net : db.nets) {
        for (const auto& pin : net.pins) {
            if (!acceptPin(pin)) {
                ++pin_id;
                continue;
            }
            int ix = -1;
            int iy = -1;
            int cid = locateCell(pin.x, pin.y, ix, iy);
            if (cid >= 0 && cid < static_cast<int>(cells_.size())) {
                cells_[cid].pin_ids.push_back(pin_id);
            }
            ++pin_id;
        }
    }
}

std::vector<ObstacleRect> RoutingGridBase::collectObstaclesForCurrentDie(const RouterDB& /*db*/) const
{
    // NOTE: 当前 RouterDB 未公开 obstacle 结构；这里先返回空。
    // TODO: RouterDB 增加 obstacle + die 字段后，在此进行按 die 的过滤。
    return {};
}

void TopRoutingGrid::build(const RouterDB& db, const std::vector<Point2D>& hbt_points)
{
    RoutingGridBase::build(db, hbt_points);
}

bool TopRoutingGrid::acceptPin(const Pin& pin) const
{
    // 保持兼容：unknown pin 仍允许进入 top。
    // 2D net 的真实 die 归属优先由 RouterDB + PDTreeRouter 在上层处理。
    return pin.die == DieId::kTop || pin.die == DieId::kUnknown;
}

void BottomRoutingGrid::build(const RouterDB& db, const std::vector<Point2D>& hbt_points)
{
    RoutingGridBase::build(db, hbt_points);
}

bool BottomRoutingGrid::acceptPin(const Pin& pin) const
{
    // 与 top 对称保留 unknown 兼容；2D die 解析在 RouterDB/PDTreeRouter 完成。
    return pin.die == DieId::kBottom || pin.die == DieId::kUnknown;
}

void HBTGrid::build(const RouterDB& db, int origin_x, int origin_y)
{
    slots_.clear();
    bucket_to_slots_.clear();
    origin_x_ = origin_x;
    origin_y_ = origin_y;
    pitch_x_ = std::max(1, db.hbt.pitch_x);
    pitch_y_ = std::max(1, db.hbt.pitch_y);

    if (!db.hbt.found || db.hbt.pitch_x <= 0 || db.hbt.pitch_y <= 0) {
        std::cerr << "[HBTGrid::build] HBT not found or invalid pitch. found="
                  << db.hbt.found << " pitch_x=" << db.hbt.pitch_x
                  << " pitch_y=" << db.hbt.pitch_y << std::endl;
        return;
    }

    // origin 逻辑说明：
    // - 当前 RouterDB 无 HBT origin 字段，因此由调用方显式传入。
    // - 典型默认值可取 die_lx/die_ly。
    // - 当后续 LEF/DEF 解析可提供真实 origin 时，替换本接口传参即可。
    int id = 0;
    for (int x = origin_x; x <= db.die_ux; x += db.hbt.pitch_x) {
        if (x < db.die_lx) {
            continue;
        }
        for (int y = origin_y; y <= db.die_uy; y += db.hbt.pitch_y) {
            if (y < db.die_ly) {
                continue;
            }
            HBTSlot s;
            s.id = id++;
            s.x = x;
            s.y = y;
            s.occupied = false;
            slots_.push_back(s);
        }
    }
    rebuildSpatialBuckets();
}

long long HBTGrid::makeBucketKey(int bx, int by) const
{
    return (static_cast<long long>(bx) << 32) ^ static_cast<unsigned int>(by);
}

void HBTGrid::rebuildSpatialBuckets()
{
    bucket_to_slots_.clear();
    for (const auto& s : slots_) {
        const int bx = (s.x - origin_x_) / std::max(1, pitch_x_);
        const int by = (s.y - origin_y_) / std::max(1, pitch_y_);
        bucket_to_slots_[makeBucketKey(bx, by)].push_back(s.id);
    }
}

std::vector<int> HBTGrid::querySlotsInBox(int lx, int ly, int ux, int uy) const
{
    std::vector<int> out;
    if (slots_.empty()) {
        return out;
    }

    const int minx = std::min(lx, ux);
    const int maxx = std::max(lx, ux);
    const int miny = std::min(ly, uy);
    const int maxy = std::max(ly, uy);
    const int bx0 = (minx - origin_x_) / std::max(1, pitch_x_);
    const int bx1 = (maxx - origin_x_) / std::max(1, pitch_x_);
    const int by0 = (miny - origin_y_) / std::max(1, pitch_y_);
    const int by1 = (maxy - origin_y_) / std::max(1, pitch_y_);
    for (int bx = bx0; bx <= bx1; ++bx) {
        for (int by = by0; by <= by1; ++by) {
            const auto it = bucket_to_slots_.find(makeBucketKey(bx, by));
            if (it == bucket_to_slots_.end()) {
                continue;
            }
            for (int hid : it->second) {
                const auto& s = slots_.at(hid);
                if (s.x >= minx && s.x <= maxx && s.y >= miny && s.y <= maxy) {
                    out.push_back(hid);
                }
            }
        }
    }
    return out;
}

std::vector<int> HBTGrid::queryNearestSlots(int x, int y, int k) const
{
    if (k <= 0 || slots_.empty()) {
        return {};
    }
    std::vector<std::pair<int, int>> dist_id;
    dist_id.reserve(slots_.size());
    for (const auto& s : slots_) {
        dist_id.push_back({std::abs(s.x - x) + std::abs(s.y - y), s.id});
    }
    std::sort(dist_id.begin(), dist_id.end());
    if (static_cast<int>(dist_id.size()) > k) {
        dist_id.resize(k);
    }
    std::vector<int> out;
    out.reserve(dist_id.size());
    for (const auto& p : dist_id) {
        out.push_back(p.second);
    }
    return out;
}

std::vector<int> HBTGrid::queryNearestSlotsToBox(int lx, int ly, int ux, int uy, int k) const
{
    if (k <= 0 || slots_.empty()) {
        return {};
    }
    const int minx = std::min(lx, ux);
    const int maxx = std::max(lx, ux);
    const int miny = std::min(ly, uy);
    const int maxy = std::max(ly, uy);
    std::vector<std::pair<int, int>> dist_id;
    dist_id.reserve(slots_.size());
    for (const auto& s : slots_) {
        const int dx = (s.x < minx) ? (minx - s.x) : ((s.x > maxx) ? (s.x - maxx) : 0);
        const int dy = (s.y < miny) ? (miny - s.y) : ((s.y > maxy) ? (s.y - maxy) : 0);
        dist_id.push_back({dx + dy, s.id});
    }
    std::sort(dist_id.begin(), dist_id.end());
    if (static_cast<int>(dist_id.size()) > k) {
        dist_id.resize(k);
    }
    std::vector<int> out;
    out.reserve(dist_id.size());
    for (const auto& p : dist_id) {
        out.push_back(p.second);
    }
    return out;
}

void HybridGrid::build(const RouterDB& db, int hbt_origin_x, int hbt_origin_y)
{
    db_ = &db;
    bindings.clear();

    hbt.build(db, hbt_origin_x, hbt_origin_y);

    std::vector<Point2D> hbt_points;
    hbt_points.reserve(hbt.getSlots().size());
    for (const auto& s : hbt.getSlots()) {
        hbt_points.push_back({s.x, s.y});
    }

    top.build(db, hbt_points);
    bottom.build(db, hbt_points);

    bindHBTToTopBottom();
}

void HybridGrid::bindHBTToTopBottom()
{
    bindings.clear();

    for (auto& c : top.cells()) {
        c.incident_hbts.clear();
    }
    for (auto& c : bottom.cells()) {
        c.incident_hbts.clear();
    }

    for (const auto& s : hbt.getSlots()) {
        int tix = -1, tiy = -1;
        int bix = -1, biy = -1;
        const int tcid = top.locateCell(s.x, s.y, tix, tiy);
        const int bcid = bottom.locateCell(s.x, s.y, bix, biy);

        if (tcid < 0 || bcid < 0) {
            std::cerr << "[HybridGrid::bindHBTToTopBottom] skip HBT#" << s.id
                      << " at (" << s.x << ", " << s.y << ") top_cell=" << tcid
                      << " bottom_cell=" << bcid << std::endl;
            continue;
        }

        HBTBinding b;
        b.hbt_id = s.id;
        b.top_ix = tix;
        b.top_iy = tiy;
        b.bottom_ix = bix;
        b.bottom_iy = biy;
        bindings.push_back(b);

        if (auto* tc = top.getCell(tix, tiy)) {
            tc->incident_hbts.push_back(s.id);
        }
        if (auto* bc = bottom.getCell(bix, biy)) {
            bc->incident_hbts.push_back(s.id);
        }
    }
}

void HybridGrid::validate(const RouterDB& db) const
{
    // 1) 每个 HBT 能在 top/bottom 找到唯一 cell
    for (const auto& s : hbt.getSlots()) {
        int tix = -1, tiy = -1;
        int bix = -1, biy = -1;
        const int tcid = top.locateCell(s.x, s.y, tix, tiy);
        const int bcid = bottom.locateCell(s.x, s.y, bix, biy);

        if (tcid < 0 || bcid < 0) {
            std::cerr << "[HybridGrid::validate] HBT locate failed: id=" << s.id
                      << " xy=(" << s.x << "," << s.y << ")"
                      << " top=" << tcid << " bottom=" << bcid << std::endl;
        }
    }

    // 2) 每个 pin 都能映射到某个 cell（按 die 选择 top/bottom）
    int net_idx = 0;
    for (const auto& net : db.nets) {
        int pin_idx = 0;
        for (const auto& pin : net.pins) {
            int ix = -1, iy = -1;
            int cid = -1;
            if (pin.die == DieId::kBottom) {
                cid = bottom.locateCell(pin.x, pin.y, ix, iy);
            } else {
                cid = top.locateCell(pin.x, pin.y, ix, iy);
            }

            if (cid < 0) {
                std::cerr << "[HybridGrid::validate] pin locate failed net=" << net.name
                          << " net_idx=" << net_idx << " pin_idx=" << pin_idx
                          << " pin=" << pin.name << " xy=(" << pin.x << "," << pin.y
                          << ") die=" << static_cast<int>(pin.die) << std::endl;
            }
            ++pin_idx;
        }
        ++net_idx;
    }

    // 3) 每个 3D net 至少一个候选 HBT（简单版：net bbox 扩窗内是否存在 HBT）
    const int window = std::max(1, std::min(db.hbt.pitch_x > 0 ? db.hbt.pitch_x : (db.die_ux - db.die_lx) / 32,
                                            db.hbt.pitch_y > 0 ? db.hbt.pitch_y : (db.die_uy - db.die_ly) / 32));

    for (const auto& net : db.nets) {
        if (!net.is_3d || net.pins.empty()) {
            continue;
        }

        int lx = std::numeric_limits<int>::max();
        int ly = std::numeric_limits<int>::max();
        int ux = std::numeric_limits<int>::min();
        int uy = std::numeric_limits<int>::min();

        for (const auto& p : net.pins) {
            lx = std::min(lx, p.x);
            ly = std::min(ly, p.y);
            ux = std::max(ux, p.x);
            uy = std::max(uy, p.y);
        }

        lx -= window;
        ly -= window;
        ux += window;
        uy += window;

        bool found_candidate = false;
        for (const auto& s : hbt.getSlots()) {
            if (s.x >= lx && s.x <= ux && s.y >= ly && s.y <= uy) {
                found_candidate = true;
                break;
            }
        }

        if (!found_candidate) {
            std::cerr << "[HybridGrid::validate] no candidate HBT for 3D net=" << net.name
                      << " bbox=(" << lx << "," << ly << ")-(" << ux << "," << uy << ")"
                      << std::endl;
        }
    }
}

/*
后续可扩展方向：
1) edge capacity：
   - 在 GridCell 邻接边上加入方向性容量，并关联 routing layer capacity profile。
2) HBT occupancy update：
   - 在全局布线提交路径时实时更新 HBTSlot::occupied / owner / demand。
3) 3D net 候选 HBT 搜索：
   - 从当前 bbox 扩窗升级为多目标代价搜索（拥塞 + 线长 + 时序）。
4) timing-driven cost：
   - 将 RC、slew、criticality 注入 HybridGrid 的边代价评估。
5) top/bottom layer 分离：
   - RouterDB 扩展 per-die routing_layers / obstacles 后，在 RoutingGridBase
     的筛选接口内按 die 独立统计代表性 pitch 与 blockages。
*/
