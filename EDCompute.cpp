#include "EDCompute.h"

#include <algorithm>
#include <iostream>
#include <vector>

namespace {

bool isValidTreeIndex(const NetRouteResult& result, int idx)
{
    return idx >= 0 && idx < static_cast<int>(result.tree_nodes.size());
}

void markDelayFailure(NetRouteResult& result, const std::string& status, const std::string& reason)
{
    result.delay_summary = NetDelaySummary{};
    result.delay_summary.ready = false;
    result.delay_summary.status = status;
    result.delay_summary.fail_reason = reason;
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
    result.delay_summary = NetDelaySummary{};
    result.delay_summary.status = "running";

    if (!result.success || result.status == "invalid_topology") {
        markDelayFailure(result, "edcompute_invalid_tree", "route status is not success or topology is invalid");
        return false;
    }

    if (net.pins.empty() || result.tree_nodes.empty() || !isValidTreeIndex(result, result.root_tree_index)) {
        markDelayFailure(result, "edcompute_invalid_tree", "empty net/tree or invalid root");
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
            markDelayFailure(result, "edcompute_invalid_tree", "invalid parent index");
            return false;
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
        result.delay_summary.status = "ok";
        result.delay_summary.avg_sink_delay = 0.0;
        result.delay_summary.max_sink_delay = 0.0;
        return true;
    }

    std::vector<double> local_cap(n, 0.0);
    const int root_pin = result.tree_nodes[root_idx].pin_index;
    int mapped_sink_count = 0;
    int expected_sink_count = 0;
    for (int pin_idx = 0; pin_idx < static_cast<int>(net.pins.size()); ++pin_idx) {
        if (pin_idx == root_pin) {
            continue;
        }
        ++expected_sink_count;
        const int t = pin_to_tree[pin_idx];
        if (!isValidTreeIndex(result, t)) {
            continue;
        }
        ++mapped_sink_count;
        const Pin& pin = net.pins[pin_idx];
        const double cap = pin.has_input_cap ? std::max(0.0, pin.input_cap) : params_.default_sink_cap;
        local_cap[t] += cap;
        result.delay_summary.total_load_cap += cap;
    }

    // All sink pins must map to tree nodes, otherwise avg/max delay is incomplete.
    if (expected_sink_count <= 0) {
        markDelayFailure(result, "edcompute_empty_sink", "net has no sink pin after excluding root");
        return false;
    }
    if (mapped_sink_count != expected_sink_count) {
        markDelayFailure(result, "edcompute_unmapped_sink", "not all sinks are mapped into routing tree");
        return false;
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
            const double edge_wire_cap = std::max(0.0, child.incoming_wire_cap);
            const double hbt_cap = (child.incoming_hbt_count > 0)
                                       ? ((child.incoming_hbt_cap >= 0.0)
                                              ? child.incoming_hbt_cap
                                              : params_.default_hbt_cap * static_cast<double>(child.incoming_hbt_count))
                                       : 0.0;
            if (child.incoming_hbt_count > 0 && hbt_cap < 0.0) {
                markDelayFailure(result, "edcompute_invalid_tree", "invalid HBT cap and no valid fallback");
                return false;
            }
            // subtree_cap[u] = local sink cap at u + all child subtree + wire cap + HBT lumped cap.
            // Convention: HBT capacitance is modeled as lumped cap on child-side of cross-die edge.
            cap += subtree_cap[v] + edge_wire_cap + std::max(0.0, hbt_cap);
            result.delay_summary.total_wire_cap += edge_wire_cap;
            result.delay_summary.total_hbt_cap += std::max(0.0, hbt_cap);
        }
        subtree_cap[u] = cap;
    }
    result.delay_summary.total_tree_cap = subtree_cap[root_idx];

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
            const double wire_res = std::max(0.0, child.incoming_wire_res);
            const double wire_cap = std::max(0.0, child.incoming_wire_cap);
            const double hbt_res = (child.incoming_hbt_count > 0)
                                       ? ((child.incoming_hbt_res > 0.0)
                                              ? child.incoming_hbt_res
                                              : params_.default_hbt_res * static_cast<double>(child.incoming_hbt_count))
                                       : 0.0;
            const double hbt_cap = (child.incoming_hbt_count > 0)
                                       ? ((child.incoming_hbt_cap >= 0.0)
                                              ? child.incoming_hbt_cap
                                              : params_.default_hbt_cap * static_cast<double>(child.incoming_hbt_count))
                                       : 0.0;
            if (child.incoming_hbt_count > 0 && hbt_res <= 0.0) {
                markDelayFailure(result, "edcompute_invalid_tree", "invalid HBT res and no valid fallback");
                return false;
            }
            const double wire_term = wire_res * (subtree_cap[v] + 0.5 * wire_cap);
            // HBT delay term uses downstream load after HBT device.
            // hbt_cap is included in subtree_cap[v], so do not add it again here.
            const double hbt_term = hbt_res * subtree_cap[v];
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
        result.delay_summary.sink_path_lengths.push_back(static_cast<double>(info.path_length));
        result.delay_summary.sink_hbt_counts.push_back(info.hbt_count_from_root);
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
        markDelayFailure(result, "edcompute_empty_sink", "no mapped sink in tree");
        return false;
    }

    result.delay_summary.ready = true;
    result.delay_summary.status = "ok";
    result.delay_summary.sink_delays = std::move(sink_infos);
    result.delay_summary.avg_sink_delay = sum_delay / static_cast<double>(result.delay_summary.sink_delays.size());
    result.delay_summary.max_sink_delay = std::max(0.0, max_delay);
    result.delay_summary.max_delay_pin_index = max_pin_idx;
    result.delay_summary.max_delay_pin_name = max_pin_name;
    return true;
}
