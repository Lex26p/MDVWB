#include "modbus_poll_plan.h"

#include "modbus_semantic.h"

#include <algorithm>
#include <array>
#include <compare>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

struct PhysicalReadKey {
    std::uint8_t slaveId = 1;
    std::uint16_t address = 0;

    auto operator<=>(const PhysicalReadKey&) const = default;
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

void BuildSemanticBatches(ModbusDevicePollPlan& device)
{
    std::vector<PhysicalReadKey> uniqueLocations;
    uniqueLocations.reserve(device.semanticReads.size());
    for (const auto& read : device.semanticReads) {
        uniqueLocations.push_back(PhysicalReadKey{
            .slaveId = read.location.slaveId,
            .address = read.location.address,
        });
    }
    std::sort(uniqueLocations.begin(), uniqueLocations.end());
    uniqueLocations.erase(
        std::unique(uniqueLocations.begin(), uniqueLocations.end()),
        uniqueLocations.end());

    std::map<PhysicalReadKey, std::pair<std::size_t, std::uint16_t>> placement;
    for (const auto& location : uniqueLocations) {
        bool extend = false;
        if (!device.semanticBatches.empty()) {
            auto& previous = device.semanticBatches.back();
            const auto previousLast =
                static_cast<std::uint32_t>(previous.startAddress) +
                static_cast<std::uint32_t>(previous.quantity) - 1U;
            extend = previous.slaveId == location.slaveId &&
                previousLast < std::numeric_limits<std::uint16_t>::max() &&
                location.address == previousLast + 1U &&
                previous.quantity < 125U;
        }

        if (extend) {
            auto& batch = device.semanticBatches.back();
            const std::uint16_t offset = batch.quantity;
            ++batch.quantity;
            placement.emplace(
                location,
                std::pair{device.semanticBatches.size() - 1U, offset});
        }
        else {
            device.semanticBatches.push_back(ModbusSemanticReadBatch{
                .slaveId = location.slaveId,
                .startAddress = location.address,
                .quantity = 1U,
            });
            placement.emplace(
                location,
                std::pair{device.semanticBatches.size() - 1U, 0U});
        }
    }

    for (auto& read : device.semanticReads) {
        const auto iterator = placement.find(PhysicalReadKey{
            .slaveId = read.location.slaveId,
            .address = read.location.address,
        });
        if (iterator == placement.end()) {
            throw std::logic_error(
                "internal Modbus poll-plan batch placement is missing");
        }
        read.batchIndex = iterator->second.first;
        read.registerOffset = iterator->second.second;
    }
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

        BuildSemanticBatches(device);

        ++result.metrics.deviceCount;
        ++result.metrics.probeTransactionsPerCycle;
        result.metrics.semanticTransactionsPerCycle +=
            device.semanticReads.size();
        result.metrics.registersRequestedPerCycle +=
            static_cast<std::size_t>(device.probe.quantity) +
            device.semanticReads.size();

        result.metrics.optimizedSemanticTransactionsPerCycle +=
            device.semanticBatches.size();
        result.metrics.optimizedRegistersRequestedPerCycle +=
            static_cast<std::size_t>(device.probe.quantity);
        for (const auto& batch : device.semanticBatches) {
            result.metrics.optimizedRegistersRequestedPerCycle += batch.quantity;
        }
        result.metrics.reusedSemanticReadsPerCycle +=
            device.semanticReads.size() -
            std::accumulate(
                device.semanticBatches.begin(),
                device.semanticBatches.end(),
                std::size_t{0},
                [](std::size_t total, const ModbusSemanticReadBatch& batch) {
                    return total + batch.quantity;
                });

        result.devices.push_back(std::move(device));
    }

    result.metrics.totalTransactionsPerCycle =
        result.metrics.probeTransactionsPerCycle +
        result.metrics.semanticTransactionsPerCycle;
    result.metrics.optimizedTotalTransactionsPerCycle =
        result.metrics.probeTransactionsPerCycle +
        result.metrics.optimizedSemanticTransactionsPerCycle;
    result.metrics.savedTransactionsPerCycle =
        result.metrics.totalTransactionsPerCycle -
        result.metrics.optimizedTotalTransactionsPerCycle;
    return result;
}

} // namespace mdv::modbus
