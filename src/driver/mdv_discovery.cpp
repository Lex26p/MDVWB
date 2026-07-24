#include "mdv_discovery.h"

#include "mdv_protocol.h"

#include <array>
#include <stdexcept>

namespace mdv {

DiscoveryResult DiscoverDevices(
    ITransactionTransport& transport,
    std::uint8_t masterId,
    int passes,
    DiscoveryStopPredicate shouldStop,
    DiscoveryPassCallback onPassStart)
{
    if (masterId > kMaxDeviceAddress) {
        throw std::out_of_range("MDV master ID must be in range 0..63");
    }
    if (passes <= 0) {
        throw std::invalid_argument("MDV discovery pass count must be positive");
    }

    DiscoveryResult result;
    std::array<bool, kMaxDeviceAddress + 1> found{};

    for (int pass = 1; pass <= passes; ++pass) {
        if (onPassStart) {
            onPassStart(pass, passes);
        }

        for (std::uint16_t rawAddress = 0;
             rawAddress <= kMaxDeviceAddress;
             ++rawAddress) {
            if (shouldStop && shouldStop()) {
                result.cancelled = true;
                break;
            }

            const auto address = static_cast<std::uint8_t>(rawAddress);
            const auto transaction = transport.Execute(
                BuildReadRequest(address, masterId));
            ++result.probes;

            if (transaction.status == TransactionStatus::Timeout) {
                ++result.timeouts;
                continue;
            }
            if (transaction.status == TransactionStatus::IoError) {
                ++result.ioErrors;
                continue;
            }
            if (!transaction.response.has_value()) {
                ++result.invalidResponses;
                continue;
            }

            const auto parsed = ParseResponse(
                *transaction.response, address, masterId);
            if (!parsed.ok || parsed.state.command != Command::Read) {
                ++result.invalidResponses;
                continue;
            }

            ++result.validResponses;
            found[address] = true;
        }

        if (result.cancelled) {
            break;
        }
        result.completedPasses = pass;
    }

    for (std::uint16_t rawAddress = 0;
         rawAddress <= kMaxDeviceAddress;
         ++rawAddress) {
        if (found[rawAddress]) {
            result.addresses.push_back(static_cast<std::uint8_t>(rawAddress));
        }
    }

    return result;
}

} // namespace mdv
