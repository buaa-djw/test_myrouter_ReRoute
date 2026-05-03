#pragma once

#include <string>
#include <unordered_map>

class LibertyData
{
public:
    bool load(const std::string& path);

    bool getInputCap(const std::string& cell,
                     const std::string& pin,
                     double& cap_out) const;

    int pinCapCount() const { return static_cast<int>(cell_pin_input_cap_.size()); }

private:
    static std::string trim(const std::string& s);

private:
    std::unordered_map<std::string, double> cell_pin_input_cap_;
};
