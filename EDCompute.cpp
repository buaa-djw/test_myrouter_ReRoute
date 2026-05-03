#include "EDCompute.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool isValidTreeIndex(const NetRouteResult& result, int idx)
{
    return idx >= 0 && idx < static_cast<int>(result.tree_nodes.size());
}

bool resolveHbtRC(const TreeNodeState& node,
                  const EDCompute::Params& params,
                  double& hbt_res,
                  double& hbt_cap,
                  std::string& reason)
{
    hbt_res = 0.0;
    hbt_cap = 0.0;

    if (node.incoming_hbt_count < 0) {
        reason = "negative incoming_hbt_count";
        return false;
    }
    if (node.incoming_hbt_count == 0) {
        hbt_res = 0.0;
        hbt_cap = 0.0;
        return true;
    }

    if (params.override_tree_hbt_rc) {
        if (params.default_hbt_res <= 0.0) {
            reason = "override_tree_hbt_rc enabled but default_hbt_res is not positive";
            return false;
        }
        if (params.default_hbt_cap < 0.0) {
            reason = "override_tree_hbt_rc enabled but default_hbt_cap is negative";
            return false;
        }
        hbt_res = params.default_hbt_res * static_cast<double>(node.incoming_hbt_count);
        hbt_cap = params.default_hbt_cap * static_cast<double>(node.incoming_hbt_count);
        return true;
    }

    // Prefer per-branch RC recorded by PDTreeRouter. If it is absent, fall back
    // to EDCompute defaults so older routing results can still be annotated.
    if (node.incoming_hbt_res > 0.0) {
        hbt_res = node.incoming_hbt_res;
    } else if (node.hbt_res > 0.0) {
        hbt_res = node.hbt_res;
    } else if (params.default_hbt_res > 0.0) {
        hbt_res = params.default_hbt_res * static_cast<double>(node.incoming_hbt_count);
    } else {
        reason = "missing HBT resistance and no positive default_hbt_res";
        return false;
    }

    if (node.incoming_hbt_cap >= 0.0) {
        hbt_cap = node.incoming_hbt_cap;
    } else if (node.hbt_cap >= 0.0) {
        hbt_cap = node.hbt_cap;
    } else if (params.default_hbt_cap >= 0.0) {
        hbt_cap = params.default_hbt_cap * static_cast<double>(node.incoming_hbt_count);
    } else {
        reason = "negative HBT capacitance and no valid default_hbt_cap";
        return false;
    }

    return true;
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
    result.delay_summary.pin_count = static_cast<int>(net.pins.size());
    result.delay_summary.expected_sink_count = std::max(0, static_cast<int>(net.pins.size()) - 1);
    result.delay_summary.single_pin_net = (result.delay_summary.expected_sink_count == 0);

    auto fail = [&](const std::string& status, const std::string& reason) -> bool {
        result.delay_summary.ready = false;
        result.delay_summary.status = status;
        result.delay_summary.fail_reason = reason;
        if (params_.verbose) {
            std::cerr << "[EDCompute] " << status << " net=" << net.name
                      << " reason=" << reason << std::endl;
        }
        return false;
    };

    // Invalid topology must not produce timing-ready delay.
    if (!result.success || result.status == "invalid_topology") {
        return fail("edcompute_invalid_tree", "routing result is not successful or is marked invalid_topology");
    }

    if (net.pins.empty()) {
        return fail("edcompute_empty_net", "net has no pins");
    }
    if (result.tree_nodes.empty()) {
        return fail("edcompute_invalid_tree", "routing tree is empty");
    }
    if (!isValidTreeIndex(result, result.root_tree_index)) {
        return fail("edcompute_invalid_tree", "root_tree_index is out of range");
    }

    const int n = static_cast<int>(result.tree_nodes.size());
    const int root_idx = result.root_tree_index;
    const int root_pin = result.tree_nodes[root_idx].pin_index;
    if (root_pin < 0 || root_pin >= static_cast<int>(net.pins.size())) {
        return fail("edcompute_invalid_tree", "root tree node is not mapped to a valid source pin");
    }

    std::vector<std::vector<int>> children(n);
    std::vector<double> edge_hbt_res(n, 0.0);
    std::vector<double> edge_hbt_cap(n, 0.0);

    for (int i = 0; i < n; ++i) {
        const TreeNodeState& node = result.tree_nodes[i];

        if (node.incoming_wire_res < 0.0 || node.incoming_wire_cap < 0.0) {
            return fail("edcompute_invalid_rc", "negative incoming wire RC at tree node " + std::to_string(i));
        }

        if (i == root_idx) {
            if (node.parent_index != -1 && params_.verbose) {
                std::cerr << "[EDCompute] warning net=" << net.name
                          << " root parent_index is not -1: " << node.parent_index << std::endl;
            }
            continue;
        }

        const int p = node.parent_index;
        if (!isValidTreeIndex(result, p)) {
            return fail("edcompute_invalid_tree",
                        "invalid parent for tree node " + std::to_string(i) +
                            ", parent=" + std::to_string(p));
        }

        if (node.node_type == TreeNodeState::NodeType::kHBT && node.assigned_hbt_id < 0) {
            return fail("edcompute_invalid_hbt", "HBT tree node has no assigned_hbt_id at node " + std::to_string(i));
        }

        std::string hbt_reason;
        if (!resolveHbtRC(node, params_, edge_hbt_res[i], edge_hbt_cap[i], hbt_reason)) {
            return fail("edcompute_invalid_hbt", "invalid HBT RC at tree node " + std::to_string(i) + ": " + hbt_reason);
        }

        children[p].push_back(i);
    }

    // Pin -> tree node mapping. All non-root pins must be mapped; otherwise
    // avg/max delay would silently ignore sinks and become experimentally invalid.
    std::vector<int> pin_to_tree(net.pins.size(), -1);
    for (int i = 0; i < n; ++i) {
        const int pin_idx = result.tree_nodes[i].pin_index;
        if (pin_idx >= 0 && pin_idx < static_cast<int>(net.pins.size()) && pin_to_tree[pin_idx] < 0) {
            pin_to_tree[pin_idx] = i;
        }
    }

    int mapped_sinks = 0;
    for (int pin_idx = 0; pin_idx < static_cast<int>(net.pins.size()); ++pin_idx) {
        if (pin_idx == root_pin) {
            continue;
        }
        if (!isValidTreeIndex(result, pin_to_tree[pin_idx])) {
            result.delay_summary.mapped_sink_count = mapped_sinks;
            result.delay_summary.unmapped_sink_count = result.delay_summary.expected_sink_count - mapped_sinks;
            return fail("edcompute_unmapped_sink",
                        "sink pin is not mapped to any tree node: index=" + std::to_string(pin_idx) +
                            " name=" + net.pins[pin_idx].name);
        }
        ++mapped_sinks;
    }
    result.delay_summary.mapped_sink_count = mapped_sinks;
    result.delay_summary.unmapped_sink_count = result.delay_summary.expected_sink_count - mapped_sinks;

    // Single-pin net: valid and zero-delay by definition.
    if (net.pins.size() <= 1) {
        result.delay_summary.ready = true;
        result.delay_summary.status = "ok";
        result.delay_summary.single_pin_net = true;
        result.delay_summary.avg_sink_delay = 0.0;
        result.delay_summary.max_sink_delay = 0.0;
        result.delay_summary.total_tree_cap = 0.0;
        return true;
    }

    std::vector<double> local_cap(n, 0.0);
    double total_load_cap = 0.0;
    for (int pin_idx = 0; pin_idx < static_cast<int>(net.pins.size()); ++pin_idx) {
        if (pin_idx == root_pin) {
            continue;
        }
        const int tree_idx = pin_to_tree[pin_idx];
        const Pin& pin = net.pins[pin_idx];
        const double cap = pin.has_input_cap ? std::max(0.0, pin.input_cap) : params_.default_sink_cap;
        local_cap[tree_idx] += cap;
        total_load_cap += cap;
    }

    // Pre-order traversal also checks that all nodes are reachable from root.
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
        return fail("edcompute_invalid_tree", "routing tree is disconnected or contains unreachable nodes");
    }

    std::vector<double> subtree_cap(n, 0.0);
    std::vector<double> delay(n, 0.0);
    double ed_wire_delay_contrib = 0.0;
    double ed_hbt_delay_contrib = 0.0;

    double total_wire_cap = 0.0;
    double total_hbt_cap = 0.0;
    for (int i = 0; i < n; ++i) {
        total_wire_cap += std::max(0.0, result.tree_nodes[i].incoming_wire_cap);
        total_hbt_cap += std::max(0.0, edge_hbt_cap[i]);
    }

    // subtree_cap[u] is the total capacitance seen downstream from node u,
    // excluding the incoming edge of u but including all child edge wire caps,
    // child HBT lumped caps, and sink load caps below u.
    for (int i = n - 1; i >= 0; --i) {
        const int u = order[i];
        double cap = local_cap[u];
        for (int v : children[u]) {
            const TreeNodeState& child = result.tree_nodes[v];
            cap += subtree_cap[v]
                 + std::max(0.0, child.incoming_wire_cap)
                 + std::max(0.0, edge_hbt_cap[v]);
        }
        subtree_cap[u] = cap;
    }

    // Driver/source contribution: R_driver times the total tree capacitance.
    delay[root_idx] = params_.default_driver_res * subtree_cap[root_idx];

    // Forward propagation follows the Elmore model:
    // wire: R_wire * (C_downstream_after_wire + 0.5*C_wire)
    // HBT : R_hbt  * C_downstream_after_hbt
    // Here HBT cap is treated as a lumped load at the child side of the crossing,
    // so upstream wire/driver terms see it through subtree accumulation.
    for (int u : order) {
        for (int v : children[u]) {
            const TreeNodeState& child = result.tree_nodes[v];
            const double wire_res = std::max(0.0, child.incoming_wire_res);
            const double wire_cap = std::max(0.0, child.incoming_wire_cap);
            const double hbt_res = std::max(0.0, edge_hbt_res[v]);
            const double hbt_cap = std::max(0.0, edge_hbt_cap[v]);

            const double wire_term = wire_res * (subtree_cap[v] + 0.5 * wire_cap);
            const double hbt_term = hbt_res * (subtree_cap[v] + hbt_cap);
            ed_wire_delay_contrib += wire_term;
            ed_hbt_delay_contrib += hbt_term;
            delay[v] = delay[u] + wire_term + hbt_term;
        }
    }

    std::vector<SinkDelayInfo> sink_infos;
    sink_infos.reserve(net.pins.size() - 1);

    double sum_delay = 0.0;
    double max_delay = -1.0;
    int max_pin_idx = -1;
    std::string max_pin_name;

    for (int pin_idx = 0; pin_idx < static_cast<int>(net.pins.size()); ++pin_idx) {
        if (pin_idx == root_pin) {
            continue;
        }

        const int tree_idx = pin_to_tree[pin_idx];
        SinkDelayInfo info;
        info.pin_index = pin_idx;
        info.pin_name = net.pins[pin_idx].name;
        info.delay = delay[tree_idx];
        info.path_length = result.tree_nodes[tree_idx].path_length_from_root;
        info.hbt_count_from_root = result.tree_nodes[tree_idx].hbt_count_from_root;

        sink_infos.push_back(info);
        sum_delay += info.delay;
        if (info.delay > max_delay) {
            max_delay = info.delay;
            max_pin_idx = pin_idx;
            max_pin_name = info.pin_name;
        }
    }

    if (sink_infos.empty()) {
        return fail("edcompute_empty_sink", "multi-pin net has no sink delay entries");
    }

    result.delay_summary.ready = true;
    result.delay_summary.status = "ok";
    result.delay_summary.fail_reason.clear();
    result.delay_summary.sink_delays = std::move(sink_infos);
    result.delay_summary.avg_sink_delay = sum_delay / static_cast<double>(result.delay_summary.sink_delays.size());
    result.delay_summary.max_sink_delay = std::max(0.0, max_delay);
    result.delay_summary.max_delay_pin_index = max_pin_idx;
    result.delay_summary.max_delay_pin_name = max_pin_name;
    result.delay_summary.total_wire_cap = total_wire_cap;
    result.delay_summary.total_load_cap = total_load_cap;
    result.delay_summary.total_hbt_cap = total_hbt_cap;
    result.delay_summary.total_tree_cap = subtree_cap[root_idx];
    result.delay_summary.ed_driver_delay_contrib = delay[root_idx];
    result.delay_summary.ed_wire_delay_contrib = ed_wire_delay_contrib;
    result.delay_summary.ed_hbt_delay_contrib = ed_hbt_delay_contrib;
    result.delay_summary.ed_total_delay_contrib = result.delay_summary.ed_driver_delay_contrib
        + result.delay_summary.ed_wire_delay_contrib + result.delay_summary.ed_hbt_delay_contrib;

    result.delay_summary.sink_path_lengths.clear();
    result.delay_summary.sink_hbt_counts.clear();
    result.delay_summary.sink_path_lengths.reserve(result.delay_summary.sink_delays.size());
    result.delay_summary.sink_hbt_counts.reserve(result.delay_summary.sink_delays.size());
    for (const SinkDelayInfo& info : result.delay_summary.sink_delays) {
        result.delay_summary.sink_path_lengths.push_back(static_cast<double>(info.path_length));
        result.delay_summary.sink_hbt_counts.push_back(info.hbt_count_from_root);
    }

    return true;
}
