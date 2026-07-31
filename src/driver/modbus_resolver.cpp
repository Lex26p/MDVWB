#include "modbus_resolver.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace mdv::modbus {
namespace {

[[noreturn]] void Fail(std::string message)
{
    throw ResolverError(std::move(message));
}

void ValidateRequestedLogicalAddress(std::uint8_t logicalAddress)
{
    if (logicalAddress < kMinLogicalAddress ||
        logicalAddress > kMaxLogicalAddress) {
        Fail(
            "MDVWB logical address must be in range 1..63, got " +
            std::to_string(logicalAddress));
    }
}

void ValidateProfileLogicalRange(
    std::uint8_t logicalMin,
    std::uint8_t logicalMax,
    std::string_view typeName)
{
    if (logicalMin < kMinLogicalAddress ||
        logicalMax > kMaxLogicalAddress ||
        logicalMin > logicalMax) {
        Fail(
            "profile " + std::string(typeName) +
            " logical range must be inside 1..63");
    }
}

[[nodiscard]] bool InRange(
    std::uint8_t value,
    std::uint8_t minimum,
    std::uint8_t maximum) noexcept
{
    return value >= minimum && value <= maximum;
}

void ValidateSlaveId(
    std::uint8_t slaveId,
    std::string_view typeName)
{
    if (slaveId < 1U || slaveId > 247U) {
        Fail(
            "profile " + std::string(typeName) +
            " resolved an invalid Modbus Slave ID " +
            std::to_string(slaveId));
    }
}

} // namespace

ResolverError::ResolverError(std::string message)
    : std::runtime_error(std::move(message))
{
}

std::optional<ResolvedDeviceAddress> ResolveLogicalAddress(
    const ModbusProfile& profile,
    std::uint8_t logicalAddress)
{
    ValidateRequestedLogicalAddress(logicalAddress);

    return std::visit(
        [&](const auto& addressing)
            -> std::optional<ResolvedDeviceAddress> {
            using AddressingT = std::decay_t<decltype(addressing)>;

            if constexpr (std::is_same_v<
                              AddressingT,
                              DirectSlaveAddressing>) {
                ValidateProfileLogicalRange(
                    addressing.logicalMin,
                    addressing.logicalMax,
                    "direct_slave");

                if (!InRange(
                        logicalAddress,
                        addressing.logicalMin,
                        addressing.logicalMax)) {
                    return std::nullopt;
                }

                const auto slaveId = logicalAddress;
                ValidateSlaveId(slaveId, "direct_slave");

                return ResolvedDeviceAddress{
                    .logicalAddress = logicalAddress,
                    .slaveId = slaveId,
                    .registerOffset = addressing.registerOffset,
                };
            }
            else if constexpr (std::is_same_v<
                                   AddressingT,
                                   FixedSlaveStrideAddressing>) {
                ValidateProfileLogicalRange(
                    addressing.logicalMin,
                    addressing.logicalMax,
                    "fixed_slave_stride");
                ValidateSlaveId(
                    addressing.slaveId,
                    "fixed_slave_stride");

                // Schema v1 supports only non-negative register offsets.
                // Therefore the zero-offset anchor must be the first supported
                // logical address.
                if (addressing.firstLogicalAddress !=
                    addressing.logicalMin) {
                    Fail(
                        "profile fixed_slave_stride firstLogicalAddress "
                        "must equal logicalMin in schema v1");
                }

                if (!InRange(
                        logicalAddress,
                        addressing.logicalMin,
                        addressing.logicalMax)) {
                    return std::nullopt;
                }

                const auto distance =
                    static_cast<std::uint32_t>(logicalAddress) -
                    static_cast<std::uint32_t>(
                        addressing.firstLogicalAddress);
                const auto offset =
                    distance *
                    static_cast<std::uint32_t>(
                        addressing.registerStride);

                if (offset >
                    std::numeric_limits<std::uint16_t>::max()) {
                    Fail(
                        "profile fixed_slave_stride register offset "
                        "overflows the 16-bit Modbus register range");
                }

                return ResolvedDeviceAddress{
                    .logicalAddress = logicalAddress,
                    .slaveId = addressing.slaveId,
                    .registerOffset =
                        static_cast<std::uint16_t>(offset),
                };
            }
            else {
                static_assert(
                    std::is_same_v<AddressingT, ExplicitAddressing>);

                ValidateProfileLogicalRange(
                    addressing.logicalMin,
                    addressing.logicalMax,
                    "explicit");

                if (!InRange(
                        logicalAddress,
                        addressing.logicalMin,
                        addressing.logicalMax)) {
                    return std::nullopt;
                }

                const auto iterator =
                    addressing.devices.find(logicalAddress);
                if (iterator == addressing.devices.end()) {
                    return std::nullopt;
                }

                ValidateSlaveId(
                    iterator->second.slaveId,
                    "explicit");

                return ResolvedDeviceAddress{
                    .logicalAddress = logicalAddress,
                    .slaveId = iterator->second.slaveId,
                    .registerOffset =
                        iterator->second.registerOffset,
                };
            }
        },
        profile.addressing);
}

std::optional<ResolvedRegisterLocation> ResolveRegisterLocation(
    const ModbusProfile& profile,
    std::uint8_t logicalAddress,
    const RegisterLocation& baseLocation)
{
    const auto device =
        ResolveLogicalAddress(profile, logicalAddress);
    if (!device.has_value()) {
        return std::nullopt;
    }

    const auto effective =
        static_cast<std::uint32_t>(baseLocation.address) +
        static_cast<std::uint32_t>(device->registerOffset);

    if (effective >
        std::numeric_limits<std::uint16_t>::max()) {
        Fail(
            "effective Modbus register address overflows 0..65535 "
            "for logical address " +
            std::to_string(logicalAddress));
    }

    return ResolvedRegisterLocation{
        .logicalAddress = logicalAddress,
        .slaveId = device->slaveId,
        .space = baseLocation.space,
        .address = static_cast<std::uint16_t>(effective),
    };
}

} // namespace mdv::modbus
