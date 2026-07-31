#include "mdv_modbus_bus_config.h"

#include "modbus_resolver.h"

#include <cstdint>
#include <string>

namespace mdvwb {
namespace {

[[noreturn]] void Fail(const BusConfig& bus, std::string message)
{
    throw BusesConfigError(
        "bus " + std::to_string(bus.id) + ": " + std::move(message));
}

[[nodiscard]] mdv::SerialParity ToSerialParity(BusParity parity) noexcept
{
    switch (parity) {
    case BusParity::None:
        return mdv::SerialParity::None;
    case BusParity::Even:
        return mdv::SerialParity::Even;
    case BusParity::Odd:
        return mdv::SerialParity::Odd;
    }
    return mdv::SerialParity::None;
}

void ValidateTransport(
    const BusConfig& bus,
    const ModbusBusSettings& settings,
    const mdv::modbus::ModbusProfile& profile)
{
    if (static_cast<std::uint32_t>(settings.baudRate) !=
        profile.transport.baudRate) {
        Fail(
            bus,
            "Modbus baudRate " + std::to_string(settings.baudRate) +
                " does not match profile '" + profile.id +
                "' baudRate " + std::to_string(profile.transport.baudRate));
    }

    if (static_cast<std::uint8_t>(settings.dataBits) !=
        profile.transport.dataBits) {
        Fail(
            bus,
            "Modbus dataBits " + std::to_string(settings.dataBits) +
                " does not match profile '" + profile.id +
                "' dataBits " + std::to_string(profile.transport.dataBits));
    }

    if (ToSerialParity(settings.parity) != profile.transport.parity) {
        Fail(
            bus,
            "Modbus parity '" + std::string(BusParityName(settings.parity)) +
                "' does not match profile '" + profile.id + "' transport");
    }

    if (static_cast<std::uint8_t>(settings.stopBits) !=
        profile.transport.stopBits) {
        Fail(
            bus,
            "Modbus stopBits " + std::to_string(settings.stopBits) +
                " does not match profile '" + profile.id +
                "' stopBits " + std::to_string(profile.transport.stopBits));
    }
}

void ValidateConfiguredAddresses(
    const BusConfig& bus,
    const mdv::modbus::ModbusProfile& profile)
{
    for (const int address : bus.addresses) {
        try {
            const auto resolved = mdv::modbus::ResolveLogicalAddress(
                profile,
                static_cast<std::uint8_t>(address));
            if (!resolved.has_value()) {
                Fail(
                    bus,
                    "logical address " + std::to_string(address) +
                        " is not supported by profile '" + profile.id + "'");
            }
        }
        catch (const mdv::modbus::ResolverError& error) {
            Fail(
                bus,
                "cannot resolve logical address " + std::to_string(address) +
                    " with profile '" + profile.id + "': " + error.what());
        }
    }
}

} // namespace

const mdv::modbus::ModbusProfile& ResolveModbusBusProfile(
    const BusConfig& bus,
    const mdv::modbus::ProfileCatalog& catalog)
{
    if (bus.protocol != BusProtocol::ModbusRtu) {
        Fail(bus, "profile resolution requires protocol modbus_rtu");
    }
    if (!bus.modbus.has_value()) {
        Fail(bus, "protocol modbus_rtu is missing Modbus settings");
    }

    const ModbusBusSettings& settings = *bus.modbus;
    const auto* profile = catalog.Find(settings.profileId);
    if (profile == nullptr) {
        Fail(
            bus,
            "selects unknown Modbus profile '" + settings.profileId + "'");
    }

    ValidateTransport(bus, settings, *profile);
    ValidateConfiguredAddresses(bus, *profile);
    return *profile;
}

void ValidateModbusBusProfiles(
    const BusesConfig& config,
    const mdv::modbus::ProfileCatalog& catalog)
{
    for (const BusConfig& bus : config.buses) {
        if (bus.protocol == BusProtocol::ModbusRtu) {
            static_cast<void>(ResolveModbusBusProfile(bus, catalog));
        }
    }
}

} // namespace mdvwb
