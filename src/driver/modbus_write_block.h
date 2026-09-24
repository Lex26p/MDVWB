#pragma once

#include "modbus_profile.h"

#include <map>
#include <span>
#include <stdexcept>
#include <vector>

namespace mdv::modbus {

// Preserve the native snapshot (including intermediate fan speeds and half degrees).
// Never invent a replacement for an unsupported/unknown manufacturer value.
[[nodiscard]] inline std::vector<std::uint16_t> EncodeWriteBlockSnapshot(
    const WriteBlock& block, std::span<const std::uint16_t> snapshot,
    const std::map<std::size_t, std::uint16_t>& overrides)
{
    if (snapshot.size() != block.snapshotQuantity) {
        throw std::invalid_argument("writeBlock snapshot has wrong size");
    }
    for (const auto& [offset, value] : overrides) {
        static_cast<void>(value);
        if (offset >= block.fields.size()) throw std::invalid_argument("override outside writeBlock");
    }
    std::vector<std::uint16_t> result;
    for (std::size_t i = 0; i < block.fields.size(); ++i) {
        if (const auto it = overrides.find(i); it != overrides.end()) {
            result.push_back(it->second);
            continue;
        }
        const auto& field = block.fields[i];
        if (field.sourceOffset >= snapshot.size()) throw std::invalid_argument("source outside snapshot");
        const auto raw = snapshot[field.sourceOffset];
        if (field.tenthsBit7) {
            if (raw < 10 || raw > 1000 || raw % 5U != 0) {
                throw std::invalid_argument("cannot preserve temperature at writeBlock offset " + std::to_string(i));
            }
            result.push_back(static_cast<std::uint16_t>((raw / 10U) | (raw % 10U == 5U ? 0x80U : 0U)));
        }
        else {
            const auto it = field.rawMap.find(raw);
            if (it == field.rawMap.end()) {
                throw std::invalid_argument("cannot preserve raw " + std::to_string(raw) +
                    " at writeBlock offset " + std::to_string(i));
            }
            result.push_back(it->second);
        }
    }
    return result;
}

} // namespace mdv::modbus
