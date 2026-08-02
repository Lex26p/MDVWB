#pragma once

#include "modbus_profile.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace mdv::modbus {
namespace runtime_profile_detail {

struct SemanticPointDescriptor {
    std::string_view name;
    bool ProfileCapabilities::* enabled;
};

inline constexpr std::array<SemanticPointDescriptor, 8> kSemanticPoints{{
    {"power", &ProfileCapabilities::power},
    {"mode", &ProfileCapabilities::mode},
    {"fanSpeed", &ProfileCapabilities::fanSpeed},
    {"setTemperature", &ProfileCapabilities::setTemperature},
    {"roomTemperature", &ProfileCapabilities::roomTemperature},
    {"alarmCode", &ProfileCapabilities::alarm},
    {"blinds", &ProfileCapabilities::blinds},
    {"blocked", &ProfileCapabilities::blocked},
}};

[[nodiscard]] inline std::uint32_t MaximumRegisterOffset(
    const Addressing& addressing)
{
    if (const auto* direct =
            std::get_if<DirectSlaveAddressing>(&addressing);
        direct != nullptr) {
        return direct->registerOffset;
    }

    if (const auto* stride =
            std::get_if<FixedSlaveStrideAddressing>(&addressing);
        stride != nullptr) {
        const auto distance =
            static_cast<std::uint32_t>(stride->logicalMax) -
            static_cast<std::uint32_t>(stride->firstLogicalAddress);
        return distance * static_cast<std::uint32_t>(stride->registerStride);
    }

    const auto& explicitAddressing =
        std::get<ExplicitAddressing>(addressing);
    std::uint32_t maximum = 0;
    for (const auto& [logicalAddress, location] :
         explicitAddressing.devices) {
        static_cast<void>(logicalAddress);
        maximum = std::max(
            maximum,
            static_cast<std::uint32_t>(location.registerOffset));
    }
    return maximum;
}

inline void ValidateResolvedRegisterRange(
    const ModbusProfile& profile,
    const RegisterLocation& location,
    std::uint16_t quantity,
    std::string_view description)
{
    if (quantity == 0U) {
        throw std::invalid_argument(
            "profile '" + profile.id + "' has a zero-length " +
            std::string(description));
    }

    const auto lastAddress =
        static_cast<std::uint64_t>(location.address) +
        static_cast<std::uint64_t>(
            MaximumRegisterOffset(profile.addressing)) +
        static_cast<std::uint64_t>(quantity) - 1U;

    if (lastAddress >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::uint16_t>::max())) {
        throw std::invalid_argument(
            "profile '" + profile.id + "' resolves " +
            std::string(description) +
            " outside the 16-bit Modbus register range");
    }
}

} // namespace runtime_profile_detail

// Validates the subset of profile schema v1 that the current production
// runtime and discovery executor can actually execute. The generic parser
// remains capable of representing future data spaces; production consumers
// must call this boundary before exposing or accepting a profile.
inline void ValidateModbusRuntimeProfile(const ModbusProfile& profile)
{
    if (profile.probe.read.space != RegisterSpace::HoldingRegister) {
        throw std::invalid_argument(
            "profile '" + profile.id +
            "' uses an unsupported discovery probe data space; "
            "only holding_register is supported");
    }

    runtime_profile_detail::ValidateResolvedRegisterRange(
        profile,
        profile.probe.read,
        profile.probe.quantity,
        "discovery probe");

    std::size_t readablePoints = 0;
    for (const auto& descriptor :
         runtime_profile_detail::kSemanticPoints) {
        if (!(profile.capabilities.*(descriptor.enabled))) {
            continue;
        }

        const auto iterator = profile.points.find(descriptor.name);
        if (iterator == profile.points.end()) {
            throw std::invalid_argument(
                "profile '" + profile.id +
                "' enables missing semantic point '" +
                std::string(descriptor.name) + "'");
        }
        if (!iterator->second.read.has_value()) {
            throw std::invalid_argument(
                "profile '" + profile.id +
                "' enables semantic point '" +
                std::string(descriptor.name) +
                "' without a read location");
        }
        if (iterator->second.read->space !=
            RegisterSpace::HoldingRegister) {
            throw std::invalid_argument(
                "profile '" + profile.id +
                "' uses an unsupported read data space for semantic point '" +
                std::string(descriptor.name) + "'");
        }

        runtime_profile_detail::ValidateResolvedRegisterRange(
            profile,
            *iterator->second.read,
            1U,
            "semantic point '" + std::string(descriptor.name) + "'");
        ++readablePoints;
    }

    if (readablePoints == 0U) {
        throw std::invalid_argument(
            "profile '" + profile.id +
            "' exposes no readable semantic points");
    }

    if (profile.capabilities.power) {
        const auto power = profile.points.find("power");
        if (power != profile.points.end() &&
            power->second.write.has_value()) {
            if (power->second.write->space !=
                RegisterSpace::HoldingRegister) {
                throw std::invalid_argument(
                    "profile '" + profile.id +
                    "' uses an unsupported Power write data space; "
                    "only holding_register is supported");
            }
            runtime_profile_detail::ValidateResolvedRegisterRange(
                profile,
                *power->second.write,
                1U,
                "Power write");
        }
    }
}

} // namespace mdv::modbus
