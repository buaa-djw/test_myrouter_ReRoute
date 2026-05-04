#include "HBTResourceManager.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <unordered_set>

namespace
{
long long manhattanDistance(const Point2D& a, const Point2D& b)
{
    return std::llabs(static_cast<long long>(a.x) - b.x)
         + std::llabs(static_cast<long long>(a.y) - b.y);
}
}

void HBTResourceManager::initializeFromGrid(const HybridGrid& grid)
{
    clear();
    const int slot_count = static_cast<int>(grid.hbt.getSlots().size());
    reservations_.resize(slot_count);
    for (int i = 0; i < slot_count; ++i) {
        reservations_[i].hbt_id = i;
    }
}

void HBTResourceManager::clear()
{
    reservations_.clear();
    stats_ = Stats{};
}

bool HBTResourceManager::hasValidSlot(int hbt_id) const
{
    return hbt_id >= 0 && hbt_id < static_cast<int>(reservations_.size());
}

bool HBTResourceManager::reserve(int hbt_id,
                                 const std::string& net_name,
                                 int result_index,
                                 int tree_node,
                                 int pin_index)
{
    if (!hasValidSlot(hbt_id)) {
        ++stats_.invalid_hbt_id_count;
        return false;
    }

    HBTReservation& r = reservations_[hbt_id];
    if (r.occupied && r.owner_net != net_name) {
        ++stats_.conflict_count;
        return false;
    }

    // Re-seeing the same net/HBT during rebuild is legal. Keep the latest owner metadata.
    r.occupied = true;
    r.owner_net = net_name;
    r.owner_result_index = result_index;
    r.owner_tree_node = tree_node;
    r.owner_pin_index = pin_index;
    return true;
}

bool HBTResourceManager::release(int hbt_id, const std::string& net_name)
{
    if (!hasValidSlot(hbt_id)) {
        ++stats_.invalid_hbt_id_count;
        return false;
    }

    HBTReservation& r = reservations_[hbt_id];
    if (!r.occupied) {
        return true;
    }
    if (r.owner_net != net_name) {
        ++stats_.conflict_count;
        return false;
    }

    r = HBTReservation{};
    r.hbt_id = hbt_id;
    return true;
}

bool HBTResourceManager::isFree(int hbt_id) const
{
    return hasValidSlot(hbt_id) && !reservations_[hbt_id].occupied;
}

bool HBTResourceManager::isOccupied(int hbt_id) const
{
    return hasValidSlot(hbt_id) && reservations_[hbt_id].occupied;
}

bool HBTResourceManager::isOwnedBy(int hbt_id, const std::string& net_name) const
{
    return hasValidSlot(hbt_id)
        && reservations_[hbt_id].occupied
        && reservations_[hbt_id].owner_net == net_name;
}

const HBTReservation* HBTResourceManager::getReservation(int hbt_id) const
{
    if (!hasValidSlot(hbt_id)) {
        return nullptr;
    }
    return &reservations_[hbt_id];
}

HBTResourceManager::Snapshot HBTResourceManager::makeSnapshot() const
{
    return Snapshot{reservations_, stats_};
}

void HBTResourceManager::rollback(const Snapshot& snapshot)
{
    reservations_ = snapshot.reservations;
    stats_ = snapshot.stats;
}

HBTResourceManager::Stats HBTResourceManager::collectStats() const
{
    Stats st = stats_;
    st.total_slots = static_cast<int>(reservations_.size());
    st.occupied_slots = 0;
    for (const HBTReservation& r : reservations_) {
        if (r.occupied) {
            ++st.occupied_slots;
        }
    }
    st.free_slots = st.total_slots - st.occupied_slots;
    return st;
}

bool HBTResourceManager::validateNoConflict(std::ostream* os) const
{
    const Stats st = collectStats();
    if (os != nullptr) {
        *os << "[HBTResourceManager] total=" << st.total_slots
            << " occ=" << st.occupied_slots
            << " free=" << st.free_slots
            << " conflict=" << st.conflict_count
            << " invalid_hbt_id=" << st.invalid_hbt_id_count
            << "\n";
    }
    return st.conflict_count == 0 && st.invalid_hbt_id_count == 0;
}

bool HBTResourceManager::rebuildFromRouteResults(const std::vector<NetRouteResult>& results,
                                                 std::ostream* os)
{
    for (HBTReservation& r : reservations_) {
        const int id = r.hbt_id;
        r = HBTReservation{};
        r.hbt_id = id;
    }
    stats_.conflict_count = 0;
    stats_.invalid_hbt_id_count = 0;

    for (int result_index = 0; result_index < static_cast<int>(results.size()); ++result_index) {
        const NetRouteResult& rr = results[result_index];

        for (int tree_node_index = 0;
             tree_node_index < static_cast<int>(rr.tree_nodes.size());
             ++tree_node_index) {
            const TreeNodeState& node = rr.tree_nodes[tree_node_index];
            if (node.assigned_hbt_id >= 0) {
                if (!reserve(node.assigned_hbt_id,
                             rr.net_name,
                             result_index,
                             tree_node_index,
                             node.pin_index)
                    && os != nullptr) {
                    *os << "[HBTResourceManager] warning conflict at hbt="
                        << node.assigned_hbt_id << " net=" << rr.net_name << "\n";
                }
            }
        }

        for (const RoutedSegment& seg : rr.segments) {
            if (!seg.uses_hbt || seg.hbt_id < 0) {
                continue;
            }
            if (!reserve(seg.hbt_id, rr.net_name, result_index, -1, -1) && os != nullptr) {
                *os << "[HBTResourceManager] warning conflict at hbt="
                    << seg.hbt_id << " net=" << rr.net_name << "\n";
            }
        }
    }

    return stats_.conflict_count == 0 && stats_.invalid_hbt_id_count == 0;
}

std::vector<int> HBTResourceManager::collectFreeHBTsNear(const HybridGrid& grid,
                                                         const Point2D& p,
                                                         int max_count) const
{
    std::vector<int> out;
    if (max_count <= 0) {
        return out;
    }

    std::vector<int> near = grid.hbt.queryNearestSlots(p.x, p.y, std::max(1, max_count * 4));
    for (int hbt_id : near) {
        if (isFree(hbt_id)) {
            out.push_back(hbt_id);
            if (static_cast<int>(out.size()) >= max_count) {
                return out;
            }
        }
    }

    // Fallback: scan all free slots by distance if the nearest query did not provide enough.
    std::vector<std::pair<long long, int>> ranked;
    const auto& slots = grid.hbt.getSlots();
    for (int hbt_id = 0; hbt_id < static_cast<int>(slots.size()); ++hbt_id) {
        if (!isFree(hbt_id)) {
            continue;
        }
        Point2D hp{slots[hbt_id].x, slots[hbt_id].y};
        ranked.emplace_back(manhattanDistance(p, hp), hbt_id);
    }
    std::sort(ranked.begin(), ranked.end());
    for (const auto& [dist, hbt_id] : ranked) {
        (void)dist;
        if (std::find(out.begin(), out.end(), hbt_id) != out.end()) {
            continue;
        }
        out.push_back(hbt_id);
        if (static_cast<int>(out.size()) >= max_count) {
            break;
        }
    }
    return out;
}

std::vector<int> HBTResourceManager::collectAllFreeHBTs() const
{
    std::vector<int> out;
    for (const HBTReservation& r : reservations_) {
        if (!r.occupied) {
            out.push_back(r.hbt_id);
        }
    }
    return out;
}
