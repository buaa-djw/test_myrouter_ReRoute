#pragma once
#include "Grid.h"
#include "PDTreeRouter.h"
#include <ostream>
#include <string>
#include <vector>

struct HBTReservation {
    int hbt_id = -1;
    bool occupied = false;
    std::string owner_net;
    int owner_result_index = -1;
    int owner_tree_node = -1;
    int owner_pin_index = -1;
};

class HBTResourceManager {
public:
    struct Stats {
        int total_slots = 0;
        int occupied_slots = 0;
        int free_slots = 0;
        int conflict_count = 0;
        int invalid_hbt_id_count = 0;
    };
    struct Snapshot { std::vector<HBTReservation> reservations; };

    void initializeFromGrid(const HybridGrid& grid);
    void clear();
    bool reserve(int hbt_id, const std::string& net_name, int result_index, int tree_node, int pin_index);
    bool release(int hbt_id, const std::string& net_name);
    bool isFree(int hbt_id) const;
    bool isOccupied(int hbt_id) const;
    bool isOwnedBy(int hbt_id, const std::string& net_name) const;
    const HBTReservation* getReservation(int hbt_id) const;
    Snapshot makeSnapshot() const;
    void rollback(const Snapshot& snapshot);
    Stats collectStats() const;
    bool validateNoConflict(std::ostream* os = nullptr) const;
    bool rebuildFromRouteResults(const std::vector<NetRouteResult>& results, std::ostream* os = nullptr);
    std::vector<int> collectFreeHBTsNear(const HybridGrid& grid, const Point2D& p, int max_count) const;
private:
    std::vector<HBTReservation> reservations_;
    mutable Stats stats_;
};
