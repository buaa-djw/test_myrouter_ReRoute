#include "LibertyData.h"

#include <cctype>
#include <fstream>
#include <regex>
#include <string>

std::string LibertyData::trim(const std::string& s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

bool LibertyData::load(const std::string& path)
{
    std::ifstream fin(path);
    if (!fin.good()) {
        return false;
    }

    cell_pin_input_cap_.clear();

    std::string line;
    std::string cur_cell;
    std::string cur_pin;
    bool in_cell = false;
    bool in_pin = false;
    bool pin_is_input = false;

    std::regex cell_re(R"(cell\s*\(\s*([^\)\s]+)\s*\))");
    std::regex pin_re(R"(pin\s*\(\s*([^\)\s]+)\s*\))");
    std::regex dir_re(R"(direction\s*:\s*([A-Za-z_]+)\s*;)");
    std::regex cap_re(R"(capacitance\s*:\s*([0-9eE+\-.]+)\s*;)");

    while (std::getline(fin, line)) {
        auto cpos = line.find("//");
        if (cpos != std::string::npos) {
            line = line.substr(0, cpos);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        std::smatch m;
        if (std::regex_search(line, m, cell_re)) {
            cur_cell = m[1].str();
            in_cell = true;
            continue;
        }
        if (in_cell && std::regex_search(line, m, pin_re)) {
            cur_pin = m[1].str();
            in_pin = true;
            pin_is_input = false;
            continue;
        }
        if (in_pin && std::regex_search(line, m, dir_re)) {
            pin_is_input = (m[1].str() == "input");
            continue;
        }
        if (in_pin && std::regex_search(line, m, cap_re)) {
            if (pin_is_input) {
                const double cap = std::stod(m[1].str());
                cell_pin_input_cap_[cur_cell + "/" + cur_pin] = cap;
            }
            continue;
        }

        if (line == "}" || line == "};") {
            if (in_pin) {
                in_pin = false;
                cur_pin.clear();
            } else if (in_cell) {
                in_cell = false;
                cur_cell.clear();
            }
        }
    }

    return true;
}

bool LibertyData::getInputCap(const std::string& cell,
                              const std::string& pin,
                              double& cap_out) const
{
    auto it = cell_pin_input_cap_.find(cell + "/" + pin);
    if (it == cell_pin_input_cap_.end()) {
        return false;
    }
    cap_out = it->second;
    return true;
}
