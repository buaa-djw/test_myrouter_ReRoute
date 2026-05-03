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
    PDTreeRouter::TraditionalPDTreeParams traditional_pdtree;
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
