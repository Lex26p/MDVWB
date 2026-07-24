#pragma once

#include "mdv_device.h"
#include "mdv_serial.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace mdv {

enum class DriverOperation {
    PollRead,
    SetState,
    Lock,
    Unlock,
    ConfirmRead,
};

enum class DriverOutcome {
    Success,
    Timeout,
    IoError,
    InvalidResponse,
};

struct DriverResult {
    std::uint8_t address = 0;
    DriverOperation operation = DriverOperation::PollRead;
    DriverOutcome outcome = DriverOutcome::Timeout;
    std::string error;
};

struct DeviceRuntime {
    explicit DeviceRuntime(std::uint8_t address, std::uint8_t masterId = 0)
        : device(address, masterId)
    {
    }

    DeviceContext device;
    bool online = false;
    bool setQueueEntry = false;
    bool blockQueueEntry = false;
    bool confirmQueueEntry = false;

    bool desiredBlocked = false;
    bool blockPending = false;
    std::uint64_t blockRevision = 0;

    std::uint64_t successfulReads = 0;
    std::uint64_t failedReads = 0;
    std::uint64_t successfulSets = 0;
    std::uint64_t failedSets = 0;
    std::uint64_t successfulBlockCommands = 0;
    std::uint64_t failedBlockCommands = 0;
    std::uint32_t consecutiveReadFailures = 0;
    std::string lastError;
};

// Owns the transaction order for one RS-485 line. Exactly one request is sent
// per call. Confirmation reads have highest priority, then CC/CD, cached C3,
// and finally the ordinary round-robin C0 poll.
class MdvDriver {
public:
    MdvDriver(
        std::vector<std::uint8_t> addresses,
        ITransactionTransport& transport,
        std::uint8_t masterId = 0);

    [[nodiscard]] DriverResult ProcessNext();

    void SetPower(std::uint8_t address, bool power);
    void SetMode(std::uint8_t address, Mode mode);
    void SetFanSpeed(std::uint8_t address, FanSpeed speed);
    void SetTemperature(std::uint8_t address, std::uint8_t temperature);
    void SetBlinds(std::uint8_t address, bool enabled);
    void SetBlocked(std::uint8_t address, bool blocked);

    [[nodiscard]] bool HasQueuedWork() const noexcept;
    [[nodiscard]] std::size_t DeviceCount() const noexcept;
    [[nodiscard]] std::uint8_t NextPollAddress() const noexcept;

    [[nodiscard]] DeviceRuntime& DeviceByAddress(std::uint8_t address);
    [[nodiscard]] const DeviceRuntime& DeviceByAddress(std::uint8_t address) const;

private:
    [[nodiscard]] DriverResult ExecuteRead(
        DeviceRuntime& runtime,
        DriverOperation operation);
    [[nodiscard]] DriverResult ExecuteSet(DeviceRuntime& runtime);
    [[nodiscard]] DriverResult ExecuteBlock(DeviceRuntime& runtime);

    void EnqueueSet(DeviceRuntime& runtime);
    void EnqueueBlock(DeviceRuntime& runtime);
    void EnqueueConfirmation(DeviceRuntime& runtime);
    [[nodiscard]] DeviceRuntime& PopSet();
    [[nodiscard]] DeviceRuntime& PopBlock();
    [[nodiscard]] DeviceRuntime& PopConfirmation();
    [[nodiscard]] DeviceRuntime& NextPollDevice() noexcept;

    void MarkReadSuccess(DeviceRuntime& runtime) noexcept;
    void MarkReadFailure(DeviceRuntime& runtime, std::string error);
    void MarkSetSuccess(DeviceRuntime& runtime) noexcept;
    void MarkSetFailure(DeviceRuntime& runtime, std::string error);
    void MarkBlockSuccess(DeviceRuntime& runtime) noexcept;
    void MarkBlockFailure(DeviceRuntime& runtime, std::string error);

    std::vector<DeviceRuntime> devices_;
    ITransactionTransport& transport_;
    std::uint8_t masterId_ = 0;
    std::size_t nextPollIndex_ = 0;
    std::deque<std::uint8_t> setQueue_;
    std::deque<std::uint8_t> blockQueue_;
    std::deque<std::uint8_t> confirmationQueue_;
};

} // namespace mdv
