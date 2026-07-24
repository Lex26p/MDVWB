#pragma once

#include "mdv_serial.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace mdv {

struct DiscoveryResult {
    std::vector<std::uint8_t> addresses;
    int completedPasses = 0;
    std::size_t probes = 0;
    std::size_t validResponses = 0;
    std::size_t timeouts = 0;
    std::size_t ioErrors = 0;
    std::size_t invalidResponses = 0;
    bool cancelled = false;
};

using DiscoveryStopPredicate = std::function<bool()>;
using DiscoveryPassCallback = std::function<void(int pass, int totalPasses)>;

// Performs complete sequential passes over addresses 0..63. An address is
// included in the result after at least one strictly valid C0 response.
[[nodiscard]] DiscoveryResult DiscoverDevices(
    ITransactionTransport& transport,
    std::uint8_t masterId = 0,
    int passes = 3,
    DiscoveryStopPredicate shouldStop = {},
    DiscoveryPassCallback onPassStart = {});

} // namespace mdv
