#pragma once

#include "mdv_buses_config.h"
#include "modbus_profile.h"

namespace mdvwb {

// Resolves and validates one Modbus RTU bus against the loaded profile catalog.
// The returned reference remains owned by the supplied catalog.
[[nodiscard]] const mdv::modbus::ModbusProfile& ResolveModbusBusProfile(
    const BusConfig& bus,
    const mdv::modbus::ProfileCatalog& catalog);

// Validates every Modbus RTU bus while leaving legacy/explicit MDV buses alone.
void ValidateModbusBusProfiles(
    const BusesConfig& config,
    const mdv::modbus::ProfileCatalog& catalog);

} // namespace mdvwb
