#pragma once

#include "mdv_device.h"
#include "mdv_serial.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mdv {

enum class PollOutcome {
    Success,
    Timeout,
    IoError,
    InvalidResponse,
};

struct PollResult {
    std::uint8_t address = 0;
    PollOutcome outcome = PollOutcome::Timeout;
    std::string error;
};

struct DeviceRuntime {
    explicit DeviceRuntime(std::uint8_t address, std::uint8_t masterId = 0)
        : device(address, masterId)
    {
    }

    DeviceContext device;
    bool online = false;
    std::uint64_t successfulPolls = 0;
    std::uint64_t failedPolls = 0;
    std::uint32_t consecutiveFailures = 0;
    std::string lastError;
};

// Performs one strictly sequential C0 transaction at a time. The transport owns
// the fixed 150 ms pacing, while this class only selects the next device and
// updates its confirmed state.
class MdvPollingDriver {
public:
    MdvPollingDriver(
        std::vector<std::uint8_t> addresses,
        ITransactionTransport& transport,
        std::uint8_t masterId = 0);

    [[nodiscard]] PollResult PollNext();

    [[nodiscard]] std::size_t DeviceCount() const noexcept;
    [[nodiscard]] std::uint8_t NextAddress() const noexcept;

    [[nodiscard]] DeviceRuntime& DeviceByAddress(std::uint8_t address);
    [[nodiscard]] const DeviceRuntime& DeviceByAddress(std::uint8_t address) const;

private:
    void MarkSuccess(DeviceRuntime& runtime) noexcept;
    void MarkFailure(DeviceRuntime& runtime, std::string error);

    std::vector<DeviceRuntime> devices_;
    ITransactionTransport& transport_;
    std::uint8_t masterId_ = 0;
    std::size_t nextIndex_ = 0;
};

} // namespace mdv
