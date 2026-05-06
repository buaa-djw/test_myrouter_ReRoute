#include "RouteTopologyValidator.h"
#include <queue>
#include <unordered_map>

RouteTopologyValidator::Result RouteTopologyValidator::validateDesignCompleteness(const std::vector<Net>& nets, const std::vector<NetRouteResult>& results) const {
    Result r; r.expected_net_count = static_cast<int>(nets.size()); r.routed_net_count = static_cast<int>(results.size());
    std::unordered_map<std::string, int> seen;
    for (const auto& re : results) seen[re.net_name]++;
    for (const auto& net : nets) {
        if (seen.find(net.name) == seen.end()) { r.valid = false; r.has_missing_net_result = true; r.reason = "missing_net_result:" + net.name; return r; }
    }
    if (r.routed_net_count != r.expected_net_count) { r.valid = false; r.has_missing_net_result = true; r.reason = "routed_net_count_mismatch"; }
    return r;
}

RouteTopologyValidator::Result RouteTopologyValidator::validateNetTopology(const Net& net, const NetRouteResult& result, const HBTResourceManager* hbt_manager) const {
    Result r;
    if (result.tree_nodes.empty() || result.root_tree_index < 0 || result.root_tree_index >= (int)result.tree_nodes.size()) { r.valid = false; r.has_invalid_index = true; r.reason = "invalid_root"; return r; }
    std::vector<std::vector<int>> ch(result.tree_nodes.size());
    std::vector<int> indeg(result.tree_nodes.size(), 0);
    std::vector<char> seg_used(result.segments.size(), 0);
    for (int i = 0; i < (int)result.tree_nodes.size(); ++i) {
        const auto& n = result.tree_nodes[i];
        if (i == result.root_tree_index) continue;
        if (n.parent_index < 0 || n.parent_index >= (int)result.tree_nodes.size()) { r.valid = false; r.has_invalid_index = true; r.reason = "invalid_parent_index"; return r; }
        if (n.incoming_segment_begin < 0 || n.incoming_segment_count < 0 || n.incoming_segment_begin + n.incoming_segment_count > (int)result.segments.size()) {
            r.valid = false; r.has_invalid_index = true; r.reason = "invalid_segment_index"; return r;
        }
        ch[n.parent_index].push_back(i); indeg[i]++;
        for (int s = n.incoming_segment_begin; s < n.incoming_segment_begin + n.incoming_segment_count; ++s) seg_used[s] = 1;
    }
    for (int i = 0; i < (int)indeg.size(); ++i) if (i != result.root_tree_index && indeg[i] != 1) { r.valid = false; r.has_cycle = true; r.reason = "invalid_incoming_degree"; return r; }
    std::queue<int> q; q.push(result.root_tree_index); std::vector<char> vis(result.tree_nodes.size(), 0); vis[result.root_tree_index] = 1;
    while (!q.empty()) { int u=q.front(); q.pop(); for (int v : ch[u]) { if (vis[v]) { r.valid=false; r.has_cycle=true; r.reason="cycle_detected"; return r; } vis[v]=1; q.push(v);} }
    for (int i = 0; i < (int)vis.size(); ++i) if (!vis[i]) { r.valid = false; r.has_unreachable_sink = true; r.reason = "isolated_node"; return r; }
    for (int i = 0; i < (int)seg_used.size(); ++i) if (!seg_used[i]) { r.valid = false; r.has_floating_segment = true; r.reason = "floating_segment"; return r; }

    for (const auto& pin : net.pins) if (pin.is_sink) r.expected_sink_count++;
    for (const auto& n : result.tree_nodes) {
        if (n.pin_index < 0 || n.pin_index >= (int)net.pins.size()) continue;
        if (net.pins[n.pin_index].is_sink) r.reachable_sink_count++;
    }
    if (r.reachable_sink_count < r.expected_sink_count) { r.valid = false; r.has_unreachable_sink = true; r.reason = "sink_missing"; return r; }

    for (const auto& s : result.segments) {
        if (s.p1.die != s.p2.die && !s.uses_hbt) { r.valid = false; r.has_invalid_hbt_connection = true; r.reason = "cross_die_without_hbt"; return r; }
        if (s.uses_hbt) {
            if (s.hbt_id < 0) { r.valid = false; r.has_invalid_hbt_connection = true; r.reason = "invalid_hbt_id"; return r; }
            if (hbt_manager && hbt_manager->getReservation(s.hbt_id)==nullptr) { r.valid = false; r.has_invalid_hbt_connection = true; r.reason = "unknown_hbt_id"; return r; }
        }
    }
    r.reason = "ok";
    return r;
}
