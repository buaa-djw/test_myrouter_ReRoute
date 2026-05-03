#pragma once

#include "odb/db.h"
#include "RouterDB.h"

#include <string>
#include <vector>

class Parser
{
public:
    struct InputFiles
    {
        std::string upper_lef;
        std::string bottom_lef;
        std::string common_lef;
        std::string hbt_lef;
        std::string def_file;
        std::vector<std::string> extra_lefs;
    };

    Parser() = default;
    ~Parser() = default;

    bool init();
    bool readLEF(const std::vector<std::string>& lef_files);
    bool readDEF(const std::string& def_file);
    bool readDesign(const InputFiles& inputs);
    bool buildRouteDB(RouterDB& router_db) const;

    odb::dbDatabase* getDB() const { return db_; }
    odb::dbChip* getChip() const { return chip_; }
    odb::dbBlock* getBlock() const { return block_; }
    const std::vector<std::string>& getLoadedLefFiles() const { return loaded_lef_files_; }

private:
    bool createChipIfNeeded();
    odb::dbTech* getTech() const;

private:
    odb::dbDatabase* db_ = nullptr;
    odb::dbChip* chip_ = nullptr;
    odb::dbBlock* block_ = nullptr;
    std::vector<std::string> loaded_lef_files_;
};
