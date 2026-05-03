#include "TimingAnnotator.h"

namespace {

const LibertyData& pickLib(DieId die,
                           const LibertyData& upper_lib,
                           const LibertyData& bottom_lib)
{
    return (die == DieId::kBottom) ? bottom_lib : upper_lib;
}

bool shouldAnnotateInputCap(const Net& net, int pin_index)
{
    if (pin_index < 0 || pin_index >= static_cast<int>(net.pins.size())) {
        return false;
    }

    if (pin_index == net.source_pin_index) {
        return false;
    }

    const Pin& pin = net.pins[pin_index];
    if (pin.is_bterm) {
        return false;
    }

    return (pin.dir == PinDir::kInput || pin.dir == PinDir::kInout || pin.dir == PinDir::kUnknown);
}

}  // namespace

TimingAnnotationSummary TimingAnnotator::annotate(RouterDB& db,
                                                  const VerilogNetlist& vnet,
                                                  const LibertyData& upper_lib,
                                                  const LibertyData& bottom_lib) const
{
    TimingAnnotationSummary s;

    for (auto& net : db.nets) {
        net.source_pin_index = -1;
        net.source_from_verilog = false;

        const VerilogEndpoint* drv = vnet.getDriver(net.name);
        if (drv == nullptr) {
            ++s.nets_without_driver;
        } else {
            ++s.nets_with_driver;
            for (int i = 0; i < static_cast<int>(net.pins.size()); ++i) {
                const Pin& p = net.pins[i];
                if (p.node_name == drv->inst && p.name == drv->pin) {
                    net.source_pin_index = i;
                    net.source_from_verilog = true;
                    break;
                }
            }
        }

        for (int pin_idx = 0; pin_idx < static_cast<int>(net.pins.size()); ++pin_idx) {
            auto& pin = net.pins[pin_idx];
            pin.has_input_cap = false;
            pin.input_cap = 0.0;

            if (!shouldAnnotateInputCap(net, pin_idx)) {
                continue;
            }

            if (pin.node_id < 0 || pin.node_id >= static_cast<int>(db.nodes.size())) {
                ++s.pins_without_input_cap;
                continue;
            }

            const Node& n = db.nodes[pin.node_id];
            const LibertyData& lib = pickLib(n.die, upper_lib, bottom_lib);
            double cap = 0.0;
            if (lib.getInputCap(n.master_name, pin.name, cap)) {
                pin.has_input_cap = true;
                pin.input_cap = cap;
                ++s.pins_with_input_cap;
            } else {
                ++s.pins_without_input_cap;
            }
        }
    }

    return s;
}
