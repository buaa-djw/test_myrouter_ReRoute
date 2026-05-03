#include "VerilogNetlist.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>

namespace {

bool startsWith(const std::string& s, const std::string& prefix)
{
    return s.rfind(prefix, 0) == 0;
}

bool isOutputPinName(const std::string& pin_name)
{
    static const char* kLikelyOutputs[] = {
        "Y", "Z", "ZN", "Q", "QN", "O", "OUT", "X"};
    for (const char* n : kLikelyOutputs) {
        if (pin_name == n) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string VerilogNetlist::trim(const std::string& s)
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

std::string VerilogNetlist::stripBusIndex(const std::string& s)
{
    std::string t = trim(s);
    auto pos = t.find('[');
    if (pos != std::string::npos) {
        t = t.substr(0, pos);
    }
    return trim(t);
}

bool VerilogNetlist::load(const std::string& path)
{
    net_to_driver_.clear();

    std::ifstream fin(path);
    if (!fin.good()) {
        return false;
    }

    std::string line;
    std::regex conn_re(R"(\.([A-Za-z_][A-Za-z0-9_$]*)\s*\(\s*([^\)]+)\s*\))");

    while (std::getline(fin, line)) {
        auto cpos = line.find("//");
        if (cpos != std::string::npos) {
            line = line.substr(0, cpos);
        }
        line = trim(line);
        if (line.empty() || startsWith(line, "module") || startsWith(line, "endmodule")
            || startsWith(line, "wire") || startsWith(line, "input")
            || startsWith(line, "output") || startsWith(line, "assign")) {
            continue;
        }

        auto p0 = line.find('(');
        if (p0 == std::string::npos) {
            continue;
        }
        std::string head = trim(line.substr(0, p0));
        std::istringstream hss(head);
        std::string cell_name;
        std::string inst_name;
        if (!(hss >> cell_name >> inst_name) || inst_name.empty()) {
            continue;
        }

        for (auto it = std::sregex_iterator(line.begin(), line.end(), conn_re);
             it != std::sregex_iterator(); ++it) {
            std::string pin_name = (*it)[1].str();
            std::string net_name = stripBusIndex((*it)[2].str());
            if (net_name.empty()) {
                continue;
            }
            if (net_to_driver_.find(net_name) != net_to_driver_.end()) {
                continue;
            }
            if (!isOutputPinName(pin_name)) {
                continue;
            }
            net_to_driver_[net_name] = VerilogEndpoint{inst_name, pin_name, false};
        }
    }

    return true;
}

bool VerilogNetlist::hasDriver(const std::string& net_name) const
{
    return net_to_driver_.find(net_name) != net_to_driver_.end();
}

const VerilogEndpoint* VerilogNetlist::getDriver(const std::string& net_name) const
{
    auto it = net_to_driver_.find(net_name);
    if (it == net_to_driver_.end()) {
        return nullptr;
    }
    return &it->second;
}
