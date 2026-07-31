#include "modbus_scan.h"

#include "modbus_resolver.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace mdv::modbus {
namespace {

[[noreturn]] void Fail(std::string message)
{
    throw ScanPlanError(std::move(message));
}

void ValidateProbeQuantity(const ProbeDefinition& probe)
{
    if (probe.quantity < 1U || probe.quantity > 125U) {
        Fail("profile scan probe quantity must be in range 1..125");
    }
}

void ValidateProbeRegisterRange(
    std::uint8_t logicalAddress,
    std::uint16_t address,
    std::uint16_t quantity)
{
    const auto lastAddress =
        static_cast<std::uint32_t>(address) +
        static_cast<std::uint32_t>(quantity) - 1U;

    if (lastAddress > std::numeric_limits<std::uint16_t>::max()) {
        Fail(
            "scan probe register range overflows 16-bit Modbus address space "
            "for logical address " +
            std::to_string(logicalAddress));
    }
}

} // namespace

ScanPlanError::ScanPlanError(std::string message)
    : std::runtime_error(std::move(message))
{
}

ScanPlan BuildScanPlan(const ModbusProfile& profile)
{
    ValidateProbeQuantity(profile.probe);

    ScanPlan plan{};

    for (std::uint16_t logical = kMinLogicalAddress;
         logical <= kMaxLogicalAddress;
         ++logical) {
        const auto logicalAddress = static_cast<std::uint8_t>(logical);
        auto& candidate =
            plan[static_cast<std::size_t>(logicalAddress - 1U)];

        candidate.logicalAddress = logicalAddress;

        try {
            const auto location = ResolveRegisterLocation(
                profile,
                logicalAddress,
                profile.probe.read);

            if (!location.has_value()) {
                candidate.probe.reset();
                continue;
            }

            ValidateProbeRegisterRange(
                logicalAddress,
                location->address,
                profile.probe.quantity);

            candidate.probe = ScanProbe{
                .logicalAddress = logicalAddress,
                .slaveId = location->slaveId,
                .space = location->space,
                .address = location->address,
                .quantity = profile.probe.quantity,
                .presence = profile.probe.presence,
            };
        }
        catch (const ResolverError& error) {
            Fail(
                "cannot build scan probe for logical address " +
                std::to_string(logicalAddress) + ": " + error.what());
        }
    }

    return plan;
}

} // namespace mdv::modbus
