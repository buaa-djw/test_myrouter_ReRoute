#include "EDCompute.h"
#include "ExperimentConfig.h"
#include "ExperimentSummaryWriter.h"
#include "Grid.h"
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

const Net* findNetByName(const RouterDB& db, const std::string& net_name)
{
    for (const auto& net : db.nets) {
        if (net.name == net_name) {
            return &net;
        }
    }
    return nullptr;
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

    // Case 1: run from src/test_myrouter_PDtree
    candidates.emplace_back(fs::current_path() / "scripts" / "plot_nets.py");

    // Case 2: run from OpenROAD root
    candidates.emplace_back(fs::current_path() / "src" / "test_myrouter_PDtree" / "scripts" / "plot_nets.py");

    // Case 3: derive module root from config path: <module>/configs/*.json
    if (!cfg_path.empty()) {
        fs::path cfg_abs = fs::absolute(cfg_path);
        if (cfg_abs.has_parent_path()) {
            fs::path config_dir = cfg_abs.parent_path();
            fs::path module_dir = config_dir.parent_path();
            candidates.emplace_back(module_dir / "scripts" / "plot_nets.py");
        }
    }

    // Case 4: derive from executable path
    if (!argv0.empty()) {
        fs::path exe_path = fs::absolute(argv0);
        if (exe_path.has_parent_path()) {
            candidates.emplace_back(exe_path.parent_path() / "scripts" / "plot_nets.py");
        }
    }

    for (const auto& p : candidates) {
        if (fs::exists(p)) {
            return p;
        }
    }

    return {};
}

}  // namespace

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
    EDCompute ed(
        db,
        EDCompute::Params{
            cfg.rc.default_sink_cap,
            cfg.rc.source_res,
            scaled_hbt_res,
            scaled_hbt_cap,
            false
        }
    );

    std::unordered_map<std::string, const Net*> net_by_name;
    for (const Net& net : db.nets) {
        net_by_name[net.name] = &net;
    }

    for (NetRouteResult& result : results) {
        auto it = net_by_name.find(result.net_name);
        if (it == net_by_name.end()) {
            result.success = false;
            result.status = "missing_net_in_db";
            result.fail_reason = "cannot find matching net by name";
            result.delay_summary.ready = false;
            continue;
        }

        if (result.success && result.status != "invalid_topology") {
            (void) ed.annotateNetDelay(*it->second, result);
        } else {
            result.delay_summary.ready = false;
        }
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

    writeExperimentSummary(
        (output_dir / cfg.output.summary_report).string(),
        cfg,
        db,
        results,
        start_ts,
        end_ts,
        runtime_sec
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
