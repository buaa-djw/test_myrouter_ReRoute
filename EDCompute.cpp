#include "EDCompute.h"

#include <algorithm>
#include <iostream>
#include <vector>

namespace {

bool isValidTreeIndex(const NetRouteResult& result, int idx)
{
    return idx >= 0 && idx < static_cast<int>(result.tree_nodes.size());
}

}  // namespace

EDCompute::EDCompute(const RouterDB& db)
    : db_(db), params_()
{
}

EDCompute::EDCompute(const RouterDB& db, const Params& params)
    : db_(db), params_(params)
{
}

bool EDCompute::annotateNetDelay(const Net& net, NetRouteResult& result) const
{
    // Delay model summary:
    // - Wire edges use distributed RC approximation.
    // - HBT edges use lumped resistance + optional lumped capacitance.
    // This keeps global-routing estimation stable before detailed extraction.
    result.delay_summary = NetDelaySummary{};

    // Invalid topology must not produce timing-ready delay.
    if (!result.success || result.status == "invalid_topology") {
        return false;
    }

    if (net.pins.empty() || result.tree_nodes.empty() || !isValidTreeIndex(result, result.root_tree_index)) {
        return false;
    }

    const int n = static_cast<int>(result.tree_nodes.size());
    std::vector<std::vector<int>> children(n);
    const int root_idx = result.root_tree_index;

    for (int i = 0; i < n; ++i) {
        if (i == root_idx) {
            continue;
        }
        const int p = result.tree_nodes[i].parent_index;
        if (!isValidTreeIndex(result, p)) {
            if (params_.verbose) {
                std::cerr << "[EDCompute] invalid parent for net=" << net.name
                          << " node=" << i << " parent=" << p << std::endl;
            }
            return false;
        }
        if (result.tree_nodes[i].node_type == TreeNodeState::NodeType::kHBT) {
            if (result.tree_nodes[i].assigned_hbt_id < 0 ||
                (result.tree_nodes[i].incoming_hbt_count > 0 &&
                 (result.tree_nodes[i].incoming_hbt_res <= 0.0 || result.tree_nodes[i].incoming_hbt_cap < 0.0))) {
                if (params_.verbose) {
                    std::cerr << "[EDCompute] invalid HBT node for net=" << net.name << " node=" << i << std::endl;
                }
                return false;
            }
        }
        children[p].push_back(i);
    }

    // Pin -> tree node mapping (first occurrence kept).
    std::vector<int> pin_to_tree(net.pins.size(), -1);
    for (int i = 0; i < n; ++i) {
        const int p = result.tree_nodes[i].pin_index;
        if (p >= 0 && p < static_cast<int>(net.pins.size()) && pin_to_tree[p] < 0) {
            pin_to_tree[p] = i;
        }
    }

    // Single-pin net: valid and zero-delay by definition.
    if (net.pins.size() <= 1) {
        result.delay_summary.ready = true;
        result.delay_summary.avg_sink_delay = 0.0;
        result.delay_summary.max_sink_delay = 0.0;
        return true;
    }

    std::vector<double> local_cap(n, 0.0);
    const int root_pin = result.tree_nodes[root_idx].pin_index;
    for (int pin_idx = 0; pin_idx < static_cast<int>(net.pins.size()); ++pin_idx) {
        if (pin_idx == root_pin) {
            continue;
        }
        const int t = pin_to_tree[pin_idx];
        if (!isValidTreeIndex(result, t)) {
            continue;
        }
        const Pin& pin = net.pins[pin_idx];
        const double cap = pin.has_input_cap ? std::max(0.0, pin.input_cap) : params_.default_sink_cap;
        local_cap[t] += cap;
    }

    std::vector<double> subtree_cap(n, 0.0);
    std::vector<double> delay(n, 0.0);

    // Post-order (iterative via reverse DFS order) to compute subtree capacitance.
    std::vector<int> order;
    order.reserve(n);
    std::vector<int> stack;
    stack.push_back(root_idx);
    while (!stack.empty()) {
        const int u = stack.back();
        stack.pop_back();
        order.push_back(u);
        for (int v : children[u]) {
            stack.push_back(v);
        }
    }
    if (static_cast<int>(order.size()) != n) {
        if (params_.verbose) {
            std::cerr << "[EDCompute] disconnected tree for net=" << net.name << std::endl;
        }
        return false;
    }

    for (int i = n - 1; i >= 0; --i) {
        const int u = order[i];
        double cap = local_cap[u];
        for (int v : children[u]) {
            const TreeNodeState& child = result.tree_nodes[v];
            // Distributed wire cap is accumulated into subtree cap.
            // HBT cap is lumped at HBT-device boundary and injected separately
            // in forward propagation to avoid mixing into 0.5*wire_cap.
            const double edge_wire_cap = child.incoming_wire_cap;
            cap += subtree_cap[v] + edge_wire_cap;
        }
        subtree_cap[u] = cap;
    }

    // Root initialization uses driver resistance proxy.
    delay[root_idx] = params_.default_driver_res * subtree_cap[root_idx];

    // Pre-order delay propagation:
    // 1) Wire part uses distributed-RC approximation:
    //    wire_res * (subtree_cap[child] + 0.5*wire_cap)
    // 2) HBT part uses lumped-device approximation:
    //    hbt_res * (subtree_cap[child] + hbt_cap)
    // This separates wire distributed capacitance from HBT lumped device RC.
    for (int u : order) {
        for (int v : children[u]) {
            const TreeNodeState& child = result.tree_nodes[v];
            const double hbt_res = (child.incoming_hbt_count > 0)
                                       ? ((child.incoming_hbt_res > 0.0)
                                              ? child.incoming_hbt_res
                                              : params_.default_hbt_res * static_cast<double>(child.incoming_hbt_count))
                                       : 0.0;
            const double hbt_cap = (child.incoming_hbt_count > 0)
                                       ? ((child.incoming_hbt_cap > 0.0)
                                              ? child.incoming_hbt_cap
                                              : params_.default_hbt_cap * static_cast<double>(child.incoming_hbt_count))
                                       : 0.0;
            const double wire_res = child.incoming_wire_res;
            const double wire_cap = child.incoming_wire_cap;
            const double wire_term = wire_res * (subtree_cap[v] + 0.5 * wire_cap);
            const double hbt_term = hbt_res * (subtree_cap[v] + hbt_cap);
            delay[v] = delay[u] + wire_term + hbt_term;
        }
    }

    std::vector<SinkDelayInfo> sink_infos;
    sink_infos.reserve(net.pins.size());

    double sum_delay = 0.0;
    double max_delay = -1.0;
    int max_pin_idx = -1;
    std::string max_pin_name;

    for (int pin_idx = 0; pin_idx < static_cast<int>(net.pins.size()); ++pin_idx) {
        if (pin_idx == root_pin) {
            continue;
        }

        const int t = pin_to_tree[pin_idx];
        if (!isValidTreeIndex(result, t)) {
            continue;
        }

        SinkDelayInfo info;
        info.pin_index = pin_idx;
        info.pin_name = net.pins[pin_idx].name;
        info.delay = delay[t];
        info.path_length = result.tree_nodes[t].path_length_from_root;
        info.hbt_count_from_root = result.tree_nodes[t].hbt_count_from_root;

        sink_infos.push_back(info);
        sum_delay += info.delay;
        if (info.delay > max_delay) {
            max_delay = info.delay;
            max_pin_idx = pin_idx;
            max_pin_name = info.pin_name;
        }
    }

    // Multi-pin nets should have at least one sink; otherwise mark not-ready.
    if (sink_infos.empty()) {
        if (params_.verbose) {
            std::cerr << "[EDCompute] no sink nodes mapped for net=" << net.name << std::endl;
        }
        return false;
    }

    result.delay_summary.ready = true;
    result.delay_summary.sink_delays = std::move(sink_infos);
    result.delay_summary.avg_sink_delay = sum_delay / static_cast<double>(result.delay_summary.sink_delays.size());
    result.delay_summary.max_sink_delay = std::max(0.0, max_delay);
    result.delay_summary.max_delay_pin_index = max_pin_idx;
    result.delay_summary.max_delay_pin_name = max_pin_name;
    return true;
}
