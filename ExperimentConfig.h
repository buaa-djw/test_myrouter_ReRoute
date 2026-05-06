#pragma once
#include "Parser.h"
#include "PDTreeRouter.h"
#include <string>
#include <vector>

struct ExperimentConfig {
    struct Output {
        std::string output_dir = "results/default";
        std::string summary_report = "summary.rpt";
        std::string net_info = "net_info.txt";
        std::string plot_data_dir = "plot_data";
        std::string plot_dir = "plots";
        bool enable_plot_generation = true;
        bool dump_invalid_3d_nets = true;
        bool dump_2d_plot_data = false;
        bool dump_3d_plot_data = true;
    } output;
    struct PDTree {
        int max_nets = -1;
        int max_candidate_parents = 64;
        int max_candidate_hbts = 128;
        int beam_width_3d = 4;
        int beam_branch_candidates_3d = 6;
        int max_local_hbt_candidates = 24;
        int max_hbt_nearest_k = 16;
        bool verbose = true;
        std::string target_net_type = "all";
        bool debug_force_accept_hbt_swap = false;
    } pd_tree;
    PDTreeRouter::ReportCostParams report_cost;
    struct RC {
        double source_res = 50;
        double default_sink_cap = 0.001;
        double hbt_res = 5;
        double hbt_cap = 0.001;
        double hbt_rc_scale = 1;
        double wire_r_scale = 1;
        double wire_c_scale = 1;
        double top_wire_r_scale = 1;
        double top_wire_c_scale = 1;
        double bottom_wire_r_scale = 1;
        double bottom_wire_c_scale = 1;
    } rc;
    struct EDComputeRC {
        bool override_enable = false;
        double source_res = 50;
        double default_sink_cap = 0.001;
        double hbt_res = 5;
        double hbt_cap = 0.001;
        double hbt_rc_scale = 1;
    } edcompute_rc;
    PDTreeRouter::TraditionalPDTreeParams traditional_pdtree;

    struct ReRoute {
        bool enable = false;
        int top_k_nets = 20;
        int max_iterations_per_net = 20;
        double max_wirelength_growth_ratio = 0.05;
        int max_extra_hbts = 0;
        bool enable_reattach = true;
        bool enable_ripup = false;
        bool enable_hbt_swap = false;
        bool enable_hbt_insert = false;
        bool enable_hbt_remove = false;
        bool enable_cross_die_ripup = false;
        bool enable_cross_layer_detour = false;
        bool debug_force_accept_hbt_insert = false;
        bool debug_force_accept_hbt_remove = false;
        bool debug_force_accept_cross_die_ripup = false;
        bool debug_force_accept_cross_layer_detour = false;
        int beam_width = 4;
        bool allow_same_die_hbt_detour = false;
        int max_hbt_insert_candidates_per_branch = 8;
        int max_hbt_remove_candidates_per_branch = 8;
        int max_hbt_swap_candidates_per_branch = 8;
        double min_predicted_gain_for_hbt_insert = 0.0;
        double min_predicted_gain_for_hbt_remove = 0.0;
        double min_predicted_gain_for_hbt_swap = 0.0;
        double objective_weight_max_delay = 1.0;
        double objective_weight_avg_delay = 0.2;
        double objective_weight_wirelength_growth = 0.05;
        double objective_weight_hbt_count = 0.1;
        double objective_weight_hbt_delay = 0.2;
        double objective_weight_hbt_conflict = 1000000.0;
        bool accept_if_max_delay_improves = true;
        bool verbose = true;
        std::string target_net_type = "all";
        bool debug_force_accept_hbt_swap = false;
    } reroute;
    struct Debug {
        bool dump_candidate_cost = false;
        bool dump_per_net_detail = true;
        bool dump_plot_data = true;
    } debug;
    std::string experiment_name = "default_exp", benchmark = "unknown", cost_mode = "report";
    Parser::InputFiles input;

    static bool loadFromFile(const std::string& path, ExperimentConfig& cfg, std::string& err);
    bool validate(std::string& err) const;
    std::string dumpJsonString() const;
    PDTreeRouter::Params buildRouterParams() const;
};
