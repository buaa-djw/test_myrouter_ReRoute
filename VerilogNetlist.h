#pragma once

#include <string>
#include <unordered_map>

struct VerilogEndpoint
{
    std::string inst;
    std::string pin;
    bool is_port = false;
};

class VerilogNetlist
{
public:
    bool load(const std::string& path);

    bool hasDriver(const std::string& net_name) const;
    const VerilogEndpoint* getDriver(const std::string& net_name) const;

    int driverCount() const { return static_cast<int>(net_to_driver_.size()); }

private:
    static std::string trim(const std::string& s);
    static std::string stripBusIndex(const std::string& s);

private:
    std::unordered_map<std::string, VerilogEndpoint> net_to_driver_;
};
