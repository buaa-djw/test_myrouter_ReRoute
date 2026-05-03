#include "Parser.h"

#include "odb/defin.h"
#include "odb/lefin.h"
#include "utl/Logger.h"

#include <iostream>
#include <vector>

bool Parser::init()
{
    db_ = odb::dbDatabase::create();
    chip_ = nullptr;
    block_ = nullptr;
    loaded_lef_files_.clear();
    return db_ != nullptr;
}

bool Parser::readLEF(const std::vector<std::string>& lef_files)
{
    if (!db_) {
        std::cerr << "[Parser::readLEF] db_ is null.\n";
        return false;
    }

    if (lef_files.empty()) {
        std::cerr << "[Parser::readLEF] empty LEF file list.\n";
        return false;
    }

    utl::Logger logger;
    odb::lefin lef_reader(db_, &logger, false);

    loaded_lef_files_.clear();

    odb::dbTech* tech = db_->getTech();
    int loaded_cnt = 0;
    int lib_idx = 0;

    for (const auto& lef_file : lef_files) {
        if (lef_file.empty()) {
            continue;
        }

        std::cout << "[Parser::readLEF] reading LEF: " << lef_file << std::endl;

        // 第一次：先创建 tech
        if (tech == nullptr) {
            tech = lef_reader.createTech("TECH", lef_file.c_str());
            if (tech == nullptr) {
                std::cerr << "[Parser::readLEF] failed to create tech from LEF: "
                          << lef_file << std::endl;
                return false;
            }
        }

        // 每个 LEF 都创建一个独立 lib，挂到同一个 tech 上
        std::string lib_name = "LIB_" + std::to_string(lib_idx);
        odb::dbLib* lib = lef_reader.createLib(tech, lib_name.c_str(), lef_file.c_str());

        if (lib == nullptr) {
            std::cerr << "[Parser::readLEF] failed to create lib from LEF: "
                      << lef_file << std::endl;
            return false;
        }

        loaded_lef_files_.push_back(lef_file);
        ++loaded_cnt;
        ++lib_idx;
    }

    if (loaded_cnt == 0) {
        std::cerr << "[Parser::readLEF] no valid LEF loaded.\n";
        return false;
    }

    int lib_count = 0;
    for (auto* lib : db_->getLibs()) {
        std::cout << "[Parser::readLEF] loaded lib: " << lib->getName() << std::endl;
        ++lib_count;
    }

    std::cout << "[Parser::readLEF] done. tech="
              << (db_->getTech() ? "valid" : "null")
              << ", lib_count=" << lib_count << std::endl;

    return (db_->getTech() != nullptr && lib_count > 0);
}

odb::dbTech* Parser::getTech() const
{
    if (!db_) {
        return nullptr;
    }

    odb::dbTech* tech = db_->getTech();
    if (tech != nullptr) {
        return tech;
    }

    for (auto* lib : db_->getLibs()) {
        if (lib != nullptr && lib->getTech() != nullptr) {
            return lib->getTech();
        }
    }

    return nullptr;
}

bool Parser::createChipIfNeeded()
{
    if (!db_) {
        std::cerr << "[Parser::createChipIfNeeded] db_ is null.\n";
        return false;
    }

    if (chip_ != nullptr) {
        return true;
    }

    odb::dbTech* tech = getTech();
    if (tech == nullptr) {
        std::cerr << "[Parser::createChipIfNeeded] tech is null.\n";
        return false;
    }

    chip_ = odb::dbChip::create(db_, tech, "chip");
    if (chip_ == nullptr) {
        std::cerr << "[Parser::createChipIfNeeded] failed to create chip.\n";
        return false;
    }

    return true;
}

bool Parser::readDEF(const std::string& def_file)
{
    if (!db_) {
        std::cerr << "[Parser::readDEF] db_ is null.\n";
        return false;
    }

    if (def_file.empty()) {
        std::cerr << "[Parser::readDEF] def file path is empty.\n";
        return false;
    }

    if (!createChipIfNeeded()) {
        return false;
    }

    utl::Logger logger;
    odb::defin def_reader(db_, &logger);

    std::vector<odb::dbLib*> libs;
    for (auto* lib : db_->getLibs()) {
        if (lib != nullptr) {
            libs.push_back(lib);
        }
    }

    std::cout << "[Parser::readDEF] reading DEF: " << def_file << std::endl;
    std::cout << "[Parser::readDEF] lib count = " << libs.size() << std::endl;

    def_reader.readChip(libs, def_file.c_str(), chip_);
    block_ = chip_->getBlock();

    if (block_ == nullptr) {
        std::cerr << "[Parser::readDEF] block is null after DEF import.\n";
        return false;
    }

    std::cout << "[Parser::readDEF] block loaded: " << block_->getName() << std::endl;
    return true;
}

bool Parser::readDesign(const InputFiles& inputs)
{
    if (!db_) {
        std::cerr << "[Parser::readDesign] db_ is null.\n";
        return false;
    }

    utl::Logger logger;
    odb::lefin lef_reader(db_, &logger, false);

    loaded_lef_files_.clear();

    // =========================================
    // 1. 读取 common LEF：创建 tech
    // =========================================
    if (inputs.common_lef.empty()) {
        std::cerr << "[Parser::readDesign] common LEF is empty.\n";
        return false;
    }

    std::cout << "[Parser::readDesign] loading common LEF: "
              << inputs.common_lef << std::endl;

    odb::dbTech* tech = lef_reader.createTech("TECH", inputs.common_lef.c_str());
    if (tech == nullptr) {
        std::cerr << "[Parser::readDesign] failed to create tech from common LEF.\n";
        return false;
    }
    loaded_lef_files_.push_back(inputs.common_lef);

    // =========================================
    // 2. 读取 HBT LEF：更新 tech
    // =========================================
    if (!inputs.hbt_lef.empty()) {
        std::cout << "[Parser::readDesign] loading HBT LEF: "
                  << inputs.hbt_lef << std::endl;

        if (!lef_reader.updateTech(tech, inputs.hbt_lef.c_str())) {
            std::cerr << "[Parser::readDesign] failed to update tech from HBT LEF.\n";
            return false;
        }
        loaded_lef_files_.push_back(inputs.hbt_lef);
    }

    // =========================================
    // 3. 读取 upper library LEF
    // =========================================
    if (!inputs.upper_lef.empty()) {
        std::cout << "[Parser::readDesign] loading upper LEF: "
                  << inputs.upper_lef << std::endl;

        odb::dbLib* upper_lib = lef_reader.createLib(tech, "UPPER_LIB", inputs.upper_lef.c_str());
        if (upper_lib == nullptr) {
            std::cerr << "[Parser::readDesign] failed to create upper lib.\n";
            return false;
        }
        loaded_lef_files_.push_back(inputs.upper_lef);
    }

    // =========================================
    // 4. 读取 bottom library LEF
    // =========================================
    if (!inputs.bottom_lef.empty()) {
        std::cout << "[Parser::readDesign] loading bottom LEF: "
                  << inputs.bottom_lef << std::endl;

        odb::dbLib* bottom_lib = lef_reader.createLib(tech, "BOTTOM_LIB", inputs.bottom_lef.c_str());
        if (bottom_lib == nullptr) {
            std::cerr << "[Parser::readDesign] failed to create bottom lib.\n";
            return false;
        }
        loaded_lef_files_.push_back(inputs.bottom_lef);
    }

    // =========================================
    // 5. 打印检查
    // =========================================
    int lib_count = 0;
    for (auto* lib : db_->getLibs()) {
        std::cout << "[Parser::readDesign] loaded lib: " << lib->getName() << std::endl;
        ++lib_count;
    }

    std::cout << "[Parser::readDesign] done. tech="
              << (db_->getTech() ? "valid" : "null")
              << ", lib_count=" << lib_count << std::endl;

    // =========================================
    // 6. 读取 DEF
    // =========================================
    return readDEF(inputs.def_file);
}

bool Parser::buildRouteDB(RouterDB& router_db) const
{
    if (db_ == nullptr) {
        std::cerr << "[Parser::buildRouteDB] db_ is null.\n";
        return false;
    }
    if (block_ == nullptr) {
        std::cerr << "[Parser::buildRouteDB] block_ is null.\n";
        return false;
    }

    router_db.clear();
    router_db.build(db_, block_, loaded_lef_files_);
    return true;
}
