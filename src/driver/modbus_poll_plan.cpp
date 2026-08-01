#include "modbus_poll_plan.h"

#include "modbus_semantic.h"

#include <array>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mdv::modbus {
namespace {

constexpr std::array<std::string_view, 8> kSemanticPointNames{
    "power",
    "mode",
    "fanSpeed",
    "setTemperature",
    "roomTemperature",
    "alarmCode",
    "blinds",
    "blocked",
};

[[nodiscard]] ResolvedRegisterLocation ResolveReadablePoint(
    const ModbusProfile& profile,
    std::uint8_t logicalAddress,
    std::string_view pointName,
    const RegisterLocation& baseLocation)
{
    std::optional<ResolvedRegisterLocation> location;
    try {
        location = ResolveRegisterLocation(
            profile,
            logicalAddress,
            baseLocation);
    }
    catch (const ResolverError& error) {
        throw std::invalid_argument(
            "profile '" + profile.id +
            "' cannot resolve semantic point '" + std::string(pointName) +
            "' for logical address " + std::to_string(logicalAddress) +
            ": " + error.what());
    }

    if (!location.has_value()) {
        throw std::invalid_argument(
            "profile '" + profile.id +
            "' does not resolve semantic point '" + std::string(pointName) +
            "' for logical address " + std::to_string(logicalAddress));
    }
    if (location->space != RegisterSpace::HoldingRegister) {
        throw std::invalid_argument(
            "profile '" + profile.id +
            "' uses an unsupported read data space for semantic point '" +
            std::string(pointName) + "'");
    }
    return *location;
}

} // namespace

ModbusPollPlan BuildModbusPollPlan(
    const ModbusProfile& profile,
    const std::vector<std::uint8_t>& logicalAddresses)
{
    if (logicalAddresses.empty()) {
        throw std::invalid_argument(
            "Modbus driver requires at least one logical address");
    }

    ScanPlan scanPlan;
    try {
        scanPlan = BuildScanPlan(profile);
    }
    catch (const ScanPlanError& error) {
        throw std::invalid_argument(
            "cannot build Modbus polling plan for profile '" +
            profile.id + "': " + error.what());
    }

    ModbusPollPlan result;
    result.devices.reserve(logicalAddresses.size());
    std::set<std::uint8_t> unique;

    for (const std::uint8_t logicalAddress : logicalAddresses) {
        if (logicalAddress < kMinLogicalAddress ||
            logicalAddress > kMaxLogicalAddress) {
            throw std::invalid_argument(
                "Modbus logical address must be in range 1..63");
        }
        if (!unique.insert(logicalAddress).second) {
            throw std::invalid_argument(
                "duplicate Modbus logical address " +
                std::to_string(logicalAddress));
        }

        const auto& candidate =
            scanPlan[static_cast<std::size_t>(logicalAddress - 1U)];
        if (!candidate.probe.has_value()) {
            throw std::invalid_argument(
                "profile '" + profile.id +
                "' does not support configured logical address " +
                std::to_string(logicalAddress));
        }

        ModbusDevicePollPlan device;
        device.logicalAddress = logicalAddress;
        device.probe = *candidate.probe;

        for (const std::string_view pointName : kSemanticPointNames) {
            if (!IsSemanticPointEnabled(profile, pointName)) {
                continue;
            }

            const auto iterator = profile.points.find(pointName);
            if (iterator == profile.points.end()) {
                throw std::invalid_argument(
                    "profile '" + profile.id +
                    "' enables missing semantic point '" +
                    std::string(pointName) + "'");
            }
            if (!iterator->second.read.has_value()) {
                throw std::invalid_argument(
                    "profile '" + profile.id +
                    "' enables semantic point '" + std::string(pointName) +
                    "' without a read location");
            }

            const ResolvedRegisterLocation location = ResolveReadablePoint(
                profile,
                logicalAddress,
                pointName,
                *iterator->second.read);
            device.semanticReads.push_back(ModbusResolvedSemanticRead{
                .pointName = std::string(pointName),
                .location = location,
            });
            if (pointName == "power") {
                device.powerRead = location;
            }
        }

        if (device.semanticReads.empty()) {
            throw std::invalid_argument(
                "profile '" + profile.id +
                "' exposes no readable semantic points");
        }

        ++result.metrics.deviceCount;
        ++result.metrics.probeTransactionsPerCycle;
        result.metrics.semanticTransactionsPerCycle +=
            device.semanticReads.size();
        result.metrics.registersRequestedPerCycle +=
            static_cast<std::size_t>(device.probe.quantity) +
            device.semanticReads.size();
        result.devices.push_back(std::move(device));
    }

    result.metrics.totalTransactionsPerCycle =
        result.metrics.probeTransactionsPerCycle +
        result.metrics.semanticTransactionsPerCycle;
    return result;
}

} // namespace mdv::modbus
