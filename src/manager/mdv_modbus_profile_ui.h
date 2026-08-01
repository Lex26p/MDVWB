#pragma once

#include "modbus_profile.h"

#include <filesystem>
#include <string>

namespace mdvwb {

inline constexpr int kModbusProfileUiSchemaVersion = 1;

[[nodiscard]] std::string SerializeModbusProfileUiCatalog(
    const mdv::modbus::ProfileCatalog& catalog);

[[nodiscard]] std::string LoadModbusProfileUiCatalog(
    const std::filesystem::path& directory);

} // namespace mdvwb
