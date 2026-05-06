#include "EDCompute.h"
#include "ExperimentConfig.h"
#include "ExperimentSummaryWriter.h"
#include "Grid.h"
#include "HBTResourceManager.h"
#include "NetReRoute.h"
#include "NetInfoWriter.h"
#include "Parser.h"
#include "PDTreeRouter.h"
#include "PlotDataWriter.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <cmath>

namespace fs = std::filesystem;

namespace {

void printUsage(const char* exe)
{
    std::cerr << "Usage: " << exe << " --config <json>\n";
}

std::string nowString()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::tm tm_buf {};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif

    std::ostringstream os;
    os << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    return os.str();
}

std::string shellQuote(const std::string& s)
{
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

fs::path findPlotScript(const std::string& cfg_path, const std::string& argv0)
{
    std::vector<fs::path> candidates;

    // Highest priority: allow explicit override without recompiling.
    if (const char* env_script = std::getenv("MYROUTER_PLOT_SCRIPT")) {
        if (*env_script != '\0') {
            candidates.emplace_back(env_script);
        }
    }

    // Prefer the script that belongs to the same module as the config file.
    // Example: src/test_myrouter_ReRoute/configs/foo.json
    //       -> src/test_myrouter_ReRoute/scripts/plot_nets.py
    if (!cfg_path.empty()) {
        fs::path cfg_abs = fs::absolute(cfg_path);
        if (cfg_abs.has_parent_path()) {
            fs::path config_dir = cfg_abs.parent_path();
            fs::path module_dir = config_dir.parent_path();
            candidates.emplace_back(module_dir / "scripts" / "plot_nets.py");
        }
    }

    // Derive from executable path when the executable is placed in the module dir.
    if (!argv0.empty()) {
        fs::path exe_path = fs::absolute(argv0);
        if (exe_path.has_parent_path()) {
            candidates.emplace_back(exe_path.parent_path() / "scripts" / "plot_nets.py");
        }
    }

    // Run from src/test_myrouter_ReRoute.
    candidates.emplace_back(fs::current_path() / "scripts" / "plot_nets.py");

    // Run from OpenROAD root. Prefer ReRoute, not PDtree.
    candidates.emplace_back(fs::current_path() / "src" / "test_myrouter_ReRoute" / "scripts" / "plot_nets.py");

    // Last compatibility fallback only. Do not let PDtree shadow ReRoute.
    candidates.emplace_back(fs::current_path() / "src" / "test_myrouter_PDtree" / "scripts" / "plot_nets.py");

    for (const auto& p : candidates) {
        if (fs::exists(p) && fs::is_regular_file(p)) {
            return p;
        }
    }

    return {};
}

}  // namespace

static void runEdcomputeSanityChecks(const std::vector<NetRouteResult>& results)
{
    for (const auto& r : results) {
        if (r.success && r.delay_summary.ready) {
            if (!std::isfinite(r.delay_summary.avg_sink_delay) || !std::isfinite(r.delay_summary.max_sink_delay)) {
                std::cerr << "[selfcheck] invalid numeric delay on net=" << r.net_name << "\n";
            }
            if (r.delay_summary.max_sink_delay < r.delay_summary.avg_sink_delay) {
                std::cerr << "[selfcheck] max < avg on net=" << r.net_name << "\n";
            }
        }
        int hbt_count = 0;
        for (const auto& s : r.segments) hbt_count += s.uses_hbt ? 1 : 0;
        if (!r.is_3d && hbt_count != 0) std::cerr << "[selfcheck] 2D net has HBT: " << r.net_name << "\n";
        if (r.is_3d && hbt_count <= 0) std::cerr << "[selfcheck] 3D net has no HBT: " << r.net_name << "\n";
    }
}

int main(int argc, char** argv)
{
    std::string cfg_path;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--config" && i + 1 < argc) {
            cfg_path = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
    }

    if (cfg_path.empty()) {
        printUsage(argv[0]);
        return 1;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const std::string start_ts = nowString();

    ExperimentConfig cfg;
    std::string err;

    if (!ExperimentConfig::loadFromFile(cfg_path, cfg, err)) {
        std::cerr << "[main] failed to load config: " << err << "\n";
        return 2;
    }

    if (!cfg.validate(err)) {
        std::cerr << "[main] invalid config: " << err << "\n";
        return 3;
    }

    std::cout << "========== Effective Config ==========\n";
    std::cout << cfg.dumpJsonString() << "\n";
    std::cout << "======================================\n";

    // ------------------------------------------------------------
    // 1. Read LEF/DEF through OpenDB-based Parser
    // ------------------------------------------------------------
    Parser parser;

    if (!parser.init()) {
        std::cerr << "[main] Parser::init() failed.\n";
        return 4;
    }

    if (!parser.readDesign(cfg.input)) {
        std::cerr << "[main] Parser::readDesign() failed.\n";
        return 5;
    }

    RouterDB db;
    if (!parser.buildRouteDB(db)) {
        std::cerr << "[main] Parser::buildRouteDB() failed.\n";
        return 6;
    }

    // ------------------------------------------------------------
    // 2. Override HBT RC from experiment config
    // ------------------------------------------------------------
    const double scaled_hbt_res = cfg.rc.hbt_res * cfg.rc.hbt_rc_scale;
    const double scaled_hbt_cap = cfg.rc.hbt_cap * cfg.rc.hbt_rc_scale;

    db.hbt.has_parasitic = true;
    db.hbt.parasitic_res = scaled_hbt_res;
    db.hbt.parasitic_cap = scaled_hbt_cap;

    std::cout << "[main] HBT RC override: R=" << scaled_hbt_res
              << " C=" << scaled_hbt_cap
              << " scale=" << cfg.rc.hbt_rc_scale << "\n";

    // ------------------------------------------------------------
    // 3. Build HybridGrid
    //
    // Current HybridGrid interface:
    //   void build(const RouterDB& db, int hbt_origin_x, int hbt_origin_y)
    //
    // Use die lower-left as default HBT grid origin.
    // ------------------------------------------------------------
    HybridGrid grid;
    const int hbt_origin_x = db.die_lx;
    const int hbt_origin_y = db.die_ly;

    grid.build(db, hbt_origin_x, hbt_origin_y);
    grid.validate(db);

    std::cout << "[main] HybridGrid built with HBT origin=("
              << hbt_origin_x << ", " << hbt_origin_y << ")\n";

    // ------------------------------------------------------------
    // 4. Run PDTreeRouter
    // ------------------------------------------------------------
    PDTreeRouter::Params router_params = cfg.buildRouterParams();

    // Ensure scaled HBT RC is definitely passed into router params.
    router_params.hbt_res = scaled_hbt_res;
    router_params.hbt_cap = scaled_hbt_cap;
    router_params.source_res = cfg.rc.source_res;
    router_params.default_sink_cap = cfg.rc.default_sink_cap;

    PDTreeRouter router(db, grid, router_params);

    PDTreeRouter::RouteRunStats stats;
    std::vector<NetRouteResult> results = router.routeSignalNets(cfg.pd_tree.max_nets, &stats);

    std::cout << "[main] Routing finished."
              << " success=" << stats.routed_success
              << " failed=" << stats.routed_failed
              << " skipped_clock=" << stats.skipped_clock
              << " skipped_special=" << stats.skipped_special
              << "\n";

    // ------------------------------------------------------------
    // 5. Annotate delay by net name (results order may differ from db.nets order).
    // ------------------------------------------------------------
    const bool use_ed_override = cfg.edcompute_rc.override_enable;
    const double ed_hbt_res = use_ed_override ? cfg.edcompute_rc.hbt_res * cfg.edcompute_rc.hbt_rc_scale : scaled_hbt_res;
    const double ed_hbt_cap = use_ed_override ? cfg.edcompute_rc.hbt_cap * cfg.edcompute_rc.hbt_rc_scale : scaled_hbt_cap;
    const double ed_source_res = use_ed_override ? cfg.edcompute_rc.source_res : cfg.rc.source_res;
    const double ed_sink_cap = use_ed_override ? cfg.edcompute_rc.default_sink_cap : cfg.rc.default_sink_cap;
    EDCompute::Params ed_params;
    ed_params.default_sink_cap = ed_sink_cap;
    ed_params.default_driver_res = ed_source_res;
    ed_params.default_hbt_res = ed_hbt_res;
    ed_params.default_hbt_cap = ed_hbt_cap;
    ed_params.override_tree_hbt_rc = use_ed_override;
    ed_params.verbose = false;
    EDCompute ed(db, ed_params);

    std::unordered_map<std::string, const Net*> net_by_name;
    for (const Net& net : db.nets) {
        net_by_name[net.name] = &net;
    }
    for (NetRouteResult& result : results) {
        result.cost_mode = cfg.cost_mode;
        auto it = net_by_name.find(result.net_name);
        if (it == net_by_name.end()) {
            result.success = false;
            result.status = "missing_net_in_db";
            result.fail_reason = "cannot find matching net by name";
            result.delay_summary.ready = false;
            continue;
        }

        if (result.success && result.status != "invalid_topology") {
            if (!ed.annotateNetDelay(*it->second, result) && result.success) {
                // Delay annotation failure should be visible in top-level status/fail_reason.
                result.status = result.delay_summary.status;
                result.fail_reason = result.delay_summary.fail_reason;
            }
        } else {
            result.delay_summary.ready = false;
            result.delay_summary.status = "edcompute_invalid_tree";
            result.delay_summary.fail_reason = "routing result is not timing-annotatable";
        }
    }

    runEdcomputeSanityChecks(results);

    CriticalNetOptimizer::OptimizationStats reroute_stats;
    HBTResourceManager hbt_manager;
    hbt_manager.initializeFromGrid(grid);
    hbt_manager.rebuildFromRouteResults(results, &std::cout);
    hbt_manager.validateNoConflict(&std::cout);
    reroute_stats.hbt_conflict_before = hbt_manager.collectStats().conflict_count;

    if (cfg.reroute.enable) {
        CriticalNetOptimizer::Params rr_params;
        rr_params.top_k_nets = cfg.reroute.top_k_nets;
        rr_params.max_iterations_per_net = cfg.reroute.max_iterations_per_net;
        rr_params.max_wirelength_growth_ratio = cfg.reroute.max_wirelength_growth_ratio;
        rr_params.max_extra_hbts = cfg.reroute.max_extra_hbts;
        rr_params.enable_reattach = cfg.reroute.enable_reattach;
        rr_params.enable_edge_relocation = cfg.reroute.enable_edge_relocation;
        rr_params.enable_ripup = cfg.reroute.enable_ripup;
        rr_params.enable_hbt_swap = cfg.reroute.enable_hbt_swap;
        rr_params.enable_hbt_insert = cfg.reroute.enable_hbt_insert;
        rr_params.enable_hbt_remove = cfg.reroute.enable_hbt_remove;
        rr_params.enable_cross_die_ripup = cfg.reroute.enable_cross_die_ripup;
        rr_params.enable_cross_die_detour = cfg.reroute.enable_cross_die_detour;
        rr_params.debug_force_accept_cross_die_ripup = cfg.reroute.debug_force_accept_cross_die_ripup;
        rr_params.debug_force_accept_cross_die_detour = cfg.reroute.debug_force_accept_cross_die_detour;
        rr_params.max_hbt_candidates_per_branch = cfg.reroute.max_hbt_candidates_per_branch;
        rr_params.debug = cfg.reroute.debug;
        rr_params.beam_width = cfg.reroute.beam_width;
        rr_params.objective_weight_max_delay = cfg.reroute.objective_weight_max_delay;
        rr_params.objective_weight_avg_delay = cfg.reroute.objective_weight_avg_delay;
        rr_params.objective_weight_wirelength_growth = cfg.reroute.objective_weight_wirelength_growth;
        rr_params.objective_weight_hbt_count = cfg.reroute.objective_weight_hbt_count;
        rr_params.objective_weight_hbt_delay = cfg.reroute.objective_weight_hbt_delay;
        rr_params.objective_weight_critical_sink_delay = cfg.reroute.objective_weight_critical_sink_delay;
        rr_params.verbose = cfg.reroute.verbose;
        CriticalNetOptimizer optimizer(db, grid, router, hbt_manager, rr_params);
        reroute_stats = optimizer.optimize(results);
        hbt_manager.rebuildFromRouteResults(results, &std::cout);
        hbt_manager.validateNoConflict(&std::cout);
        reroute_stats.hbt_conflict_after = hbt_manager.collectStats().conflict_count;
        for (NetRouteResult& result : results) {
            auto it = net_by_name.find(result.net_name);
            if (it != net_by_name.end() && result.success && result.status != "invalid_topology") {
                ed.annotateNetDelay(*it->second, result);
            }
        }
        runEdcomputeSanityChecks(results);
        std::cout << "[main][reroute] visited=" << reroute_stats.visited_nets << " improved=" << reroute_stats.improved_nets << " tried=" << reroute_stats.tried_candidates << " accepted=" << reroute_stats.accepted_candidates << "
";
    }

    // ------------------------------------------------------------
    // 6. Create output directories
    // ------------------------------------------------------------
    fs::path output_dir = cfg.output.output_dir;
    if (cfg.output.output_dir.empty() || cfg.output.output_dir == "results/default") {
        output_dir = fs::path("results") / cfg.experiment_name;
    }
    const fs::path plot_data_dir = output_dir / cfg.output.plot_data_dir;
    const fs::path plot_data_2d_dir = plot_data_dir / "2d";
    const fs::path plot_data_3d_dir = plot_data_dir / "3d";
    const fs::path plot_dir = output_dir / cfg.output.plot_dir;
    const fs::path plot_2d_dir = plot_dir / "2d";
    const fs::path plot_3d_dir = plot_dir / "3d";

    fs::create_directories(output_dir);
    fs::create_directories(plot_data_dir);
    if (cfg.output.dump_2d_plot_data) {
        fs::create_directories(plot_data_2d_dir);
    }
    if (cfg.output.dump_3d_plot_data) {
        fs::create_directories(plot_data_3d_dir);
    }
    fs::create_directories(plot_dir);
    if (cfg.output.dump_2d_plot_data) {
        fs::create_directories(plot_2d_dir);
    }
    if (cfg.output.dump_3d_plot_data) {
        fs::create_directories(plot_3d_dir);
    }

    // ------------------------------------------------------------
    // 7. Write plot data for all nets
    // ------------------------------------------------------------
    if (cfg.debug.dump_plot_data) {
        for (const auto& result : results) {
            if (result.is_3d && cfg.output.dump_3d_plot_data) {
                writeNetPlotData((plot_data_dir / "3d").string(), result);
            }
            if (!result.is_3d && cfg.output.dump_2d_plot_data) {
                writeNetPlotData((plot_data_dir / "2d").string(), result);
            }
        }
    }

    // ------------------------------------------------------------
    // 8. Write net info and summary
    // ------------------------------------------------------------
    writeNetInfo((output_dir / cfg.output.net_info).string(), db, results);

    const auto t1 = std::chrono::steady_clock::now();
    const double runtime_sec = std::chrono::duration<double>(t1 - t0).count();
    const std::string end_ts = nowString();

    CriticalNetOptimizerStatsProxy rr_proxy;
    rr_proxy.visited_nets = reroute_stats.visited_nets;
    rr_proxy.improved_nets = reroute_stats.improved_nets;
    rr_proxy.tried_candidates = reroute_stats.tried_candidates;
    rr_proxy.accepted_candidates = reroute_stats.accepted_candidates;
    rr_proxy.rejected_by_topology = reroute_stats.rejected_by_topology;
    rr_proxy.rejected_by_delay = reroute_stats.rejected_by_delay;
    rr_proxy.rejected_by_wirelength = reroute_stats.rejected_by_wirelength;
    rr_proxy.rejected_by_hbt_conflict = reroute_stats.rejected_by_hbt_conflict;
    rr_proxy.hbt_conflict_before = reroute_stats.hbt_conflict_before;
    rr_proxy.hbt_conflict_after = reroute_stats.hbt_conflict_after;
    rr_proxy.tried_cross_die_ripup_candidates = reroute_stats.tried_cross_die_ripup_candidates;
    rr_proxy.accepted_cross_die_ripup_candidates = reroute_stats.accepted_cross_die_ripup_candidates;
    rr_proxy.rejected_by_no_free_hbt = reroute_stats.rejected_by_no_free_hbt;
    rr_proxy.rejected_by_cross_die_not_supported = reroute_stats.rejected_by_cross_die_not_supported;
    rr_proxy.rejected_by_build_hbt_branch_failed = reroute_stats.rejected_by_build_hbt_branch_failed;
    rr_proxy.force_accepted_cross_die_ripup_candidates = reroute_stats.force_accepted_cross_die_ripup_candidates;
    writeExperimentSummary(
        (output_dir / cfg.output.summary_report).string(),
        cfg,
        db,
        results,
        start_ts,
        end_ts,
        runtime_sec,
        &rr_proxy
    );

    std::cout << "[main] Wrote summary: "
              << (output_dir / cfg.output.summary_report).string() << "\n";
    std::cout << "[main] Wrote net info: "
              << (output_dir / cfg.output.net_info).string() << "\n";

    // ------------------------------------------------------------
    // 9. Generate PNG plots with Python script
    // ------------------------------------------------------------
    if (cfg.output.enable_plot_generation) {
        const fs::path script = findPlotScript(cfg_path, argc > 0 ? argv[0] : "");

        if (script.empty()) {
            std::cerr << "[main] warning: plot_nets.py not found. "
                      << "Skip PNG generation.\n";
        } else {
            const std::string cmd =
                "python3 " + shellQuote(script.string())
                + " --root " + shellQuote(output_dir.string());

            std::cout << "[main] Running plot command: " << cmd << "\n";

            const int rc = std::system(cmd.c_str());
            if (rc != 0) {
                std::cerr << "[main] warning: plot script returned non-zero code: "
                          << rc << "\n";
            }
        }
    }

    std::cout << "[main] Done. Runtime = " << runtime_sec << " sec\n";
    return 0;
}
