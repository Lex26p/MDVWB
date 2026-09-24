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

[[nodiscard]] inline bool IsWritableSemanticPointName(
    std::string_view pointName) noexcept
{
    return pointName == "power" ||
        pointName == "mode" ||
        pointName == "fanSpeed" ||
        pointName == "setTemperature";
}

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

    auto maximumOffset = MaximumRegisterOffset(profile.addressing);
    if (location.registerStride) {
        const auto* stride = std::get_if<FixedSlaveStrideAddressing>(&profile.addressing);
        if (!stride) throw std::invalid_argument("location registerStride requires fixed_slave_stride");
        maximumOffset = static_cast<std::uint32_t>(stride->logicalMax - stride->firstLogicalAddress) * *location.registerStride;
    }
    const auto lastAddress =
        static_cast<std::uint64_t>(location.address) +
        static_cast<std::uint64_t>(
            maximumOffset) +
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

// Returns the effective write capability of the current production runtime,
// not merely the write declaration stored in a profile. Every exposed write
// must have both profile support and a confirmed-write state machine.
[[nodiscard]] inline bool IsModbusRuntimeWritablePoint(
    const ModbusProfile& profile,
    std::string_view pointName) noexcept
{
    if (!runtime_profile_detail::IsWritableSemanticPointName(pointName)) {
        return false;
    }

    const auto descriptor = std::find_if(
        runtime_profile_detail::kSemanticPoints.begin(),
        runtime_profile_detail::kSemanticPoints.end(),
        [pointName](const auto& candidate) {
            return candidate.name == pointName;
        });
    if (descriptor == runtime_profile_detail::kSemanticPoints.end() ||
        !(profile.capabilities.*(descriptor->enabled))) {
        return false;
    }

    const auto point = profile.points.find(pointName);
    return point != profile.points.end() &&
        point->second.read.has_value() &&
        IsReadableSpace(point->second.read->space) &&
        point->second.write.has_value() &&
        point->second.write->space == RegisterSpace::HoldingRegister;
}

// Validates the subset of profile schema v1 that the current production
// runtime and discovery executor can actually execute. The generic parser
// remains capable of representing future data spaces; production consumers
// must call this boundary before exposing or accepting a profile.
inline void ValidateModbusRuntimeProfile(const ModbusProfile& profile)
{
    if (!IsReadableSpace(profile.probe.read.space)) {
        throw std::invalid_argument(
            "profile '" + profile.id +
            "' uses an unsupported discovery probe data space; "
            "supported: holding_register, input_register, discrete_input");
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
        if (!IsReadableSpace(iterator->second.read->space)) {
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

        if (iterator->second.write.has_value() &&
            runtime_profile_detail::IsWritableSemanticPointName(
                descriptor.name)) {
            if (iterator->second.write->space !=
                RegisterSpace::HoldingRegister) {
                throw std::invalid_argument(
                    "profile '" + profile.id +
                    "' uses an unsupported write data space for semantic point '" +
                    std::string(descriptor.name) +
                    "'; only holding_register is supported");
            }
            runtime_profile_detail::ValidateResolvedRegisterRange(
                profile,
                *iterator->second.write,
                1U,
                "semantic point '" + std::string(descriptor.name) +
                    "' write");
        }
        ++readablePoints;
    }

    if (readablePoints == 0U) {
        throw std::invalid_argument(
            "profile '" + profile.id +
            "' exposes no readable semantic points");
    }

    if (profile.writeBlock) {
        const auto& block = *profile.writeBlock;
        if (!std::holds_alternative<FixedSlaveStrideAddressing>(profile.addressing) ||
            block.snapshot.space != RegisterSpace::InputRegister ||
            block.write.space != RegisterSpace::HoldingRegister ||
            block.write.writeFunction != WriteFunction::WriteMultipleRegisters ||
            block.fields.empty() || block.fields.size() > 123 ||
            block.snapshotQuantity == 0 || block.snapshotQuantity > 125 ||
            profile.confirmationTimeoutMs == 0) {
            throw std::invalid_argument("invalid snapshot writeBlock configuration");
        }
        runtime_profile_detail::ValidateResolvedRegisterRange(profile, block.snapshot, block.snapshotQuantity, "write snapshot");
        runtime_profile_detail::ValidateResolvedRegisterRange(profile, block.write, static_cast<std::uint16_t>(block.fields.size()), "write block");
        for (const auto& field : block.fields) {
            if (field.sourceOffset >= block.snapshotQuantity || field.tenthsBit7 == !field.rawMap.empty()) {
                throw std::invalid_argument("invalid writeBlock source/encoding");
            }
        }
        const auto& stride = std::get<FixedSlaveStrideAddressing>(profile.addressing);
        for (const auto& [name, point] : profile.points) {
            if (!IsModbusRuntimeWritablePoint(profile, name)) continue;
            const auto& write = *point.write;
            if (write.writeFunction != WriteFunction::WriteMultipleRegisters ||
                write.registerStride.value_or(stride.registerStride) != block.write.registerStride.value_or(stride.registerStride) ||
                write.address < block.write.address ||
                write.address >= static_cast<std::uint32_t>(block.write.address) + block.fields.size()) {
                throw std::invalid_argument("writable point is outside writeBlock");
            }
        }
    }

}

} // namespace mdv::modbus
