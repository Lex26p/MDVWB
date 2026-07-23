#include "mdv_driver.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace mdv {
namespace {

void ValidateAddresses(const std::vector<std::uint8_t>& addresses)
{
    if (addresses.empty()) {
        throw std::invalid_argument("at least one MDV device address is required");
    }

    auto sorted = addresses;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
        throw std::invalid_argument("MDV device addresses must be unique");
    }

    for (const auto address : addresses) {
        if (address > kMaxDeviceAddress) {
            throw std::out_of_range("MDV device address must be in range 0x00..0x3F");
        }
    }
}

[[nodiscard]] DriverOutcome ToDriverOutcome(TransactionStatus status) noexcept
{
    switch (status) {
    case TransactionStatus::Success:
        return DriverOutcome::Success;
    case TransactionStatus::Timeout:
        return DriverOutcome::Timeout;
    case TransactionStatus::IoError:
        return DriverOutcome::IoError;
    }
    return DriverOutcome::IoError;
}

[[nodiscard]] std::string DefaultTransactionError(TransactionStatus status)
{
    return status == TransactionStatus::Timeout
        ? "MDV response timeout"
        : "MDV serial I/O error";
}

} // namespace

MdvDriver::MdvDriver(
    std::vector<std::uint8_t> addresses,
    ITransactionTransport& transport,
    std::uint8_t masterId)
    : transport_(transport), masterId_(masterId)
{
    ValidateAddresses(addresses);
    devices_.reserve(addresses.size());
    for (const auto address : addresses) {
        devices_.emplace_back(address, masterId_);
    }
}

DriverResult MdvDriver::ProcessNext()
{
    if (!confirmationQueue_.empty()) {
        auto& runtime = PopConfirmation();
        return ExecuteRead(runtime, DriverOperation::ConfirmRead);
    }

    if (!setQueue_.empty()) {
        auto& runtime = PopSet();
        return ExecuteSet(runtime);
    }

    auto& runtime = NextPollDevice();
    return ExecuteRead(runtime, DriverOperation::PollRead);
}

void MdvDriver::SetPower(std::uint8_t address, bool power)
{
    auto& runtime = DeviceByAddress(address);
    runtime.device.ApplyPower(power);
    EnqueueSet(runtime);
}

void MdvDriver::SetMode(std::uint8_t address, Mode mode)
{
    auto& runtime = DeviceByAddress(address);
    runtime.device.ApplyMode(mode);
    EnqueueSet(runtime);
}

void MdvDriver::SetFanSpeed(std::uint8_t address, FanSpeed speed)
{
    auto& runtime = DeviceByAddress(address);
    runtime.device.ApplyFanSpeed(speed);
    EnqueueSet(runtime);
}

void MdvDriver::SetTemperature(std::uint8_t address, std::uint8_t temperature)
{
    auto& runtime = DeviceByAddress(address);
    runtime.device.ApplySetTemperature(temperature);
    EnqueueSet(runtime);
}

void MdvDriver::SetBlinds(std::uint8_t address, bool enabled)
{
    auto& runtime = DeviceByAddress(address);
    runtime.device.ApplyBlinds(enabled);
    EnqueueSet(runtime);
}

bool MdvDriver::HasQueuedWork() const noexcept
{
    return !confirmationQueue_.empty() || !setQueue_.empty();
}

std::size_t MdvDriver::DeviceCount() const noexcept
{
    return devices_.size();
}

std::uint8_t MdvDriver::NextPollAddress() const noexcept
{
    return devices_[nextPollIndex_].device.Address();
}

DeviceRuntime& MdvDriver::DeviceByAddress(std::uint8_t address)
{
    const auto found = std::find_if(
        devices_.begin(), devices_.end(),
        [address](const DeviceRuntime& runtime) {
            return runtime.device.Address() == address;
        });
    if (found == devices_.end()) {
        throw std::out_of_range("MDV device address is not configured");
    }
    return *found;
}

const DeviceRuntime& MdvDriver::DeviceByAddress(std::uint8_t address) const
{
    const auto found = std::find_if(
        devices_.begin(), devices_.end(),
        [address](const DeviceRuntime& runtime) {
            return runtime.device.Address() == address;
        });
    if (found == devices_.end()) {
        throw std::out_of_range("MDV device address is not configured");
    }
    return *found;
}

DriverResult MdvDriver::ExecuteRead(
    DeviceRuntime& runtime,
    DriverOperation operation)
{
    const auto address = runtime.device.Address();
    const auto transaction = transport_.Execute(BuildReadRequest(address, masterId_));

    if (transaction.status != TransactionStatus::Success) {
        auto error = transaction.error.empty()
            ? DefaultTransactionError(transaction.status)
            : transaction.error;
        MarkReadFailure(runtime, error);
        return DriverResult{
            address, operation, ToDriverOutcome(transaction.status), std::move(error)};
    }

    if (!transaction.response.has_value()) {
        std::string error = "successful MDV transaction has no response frame";
        MarkReadFailure(runtime, error);
        return DriverResult{
            address, operation, DriverOutcome::InvalidResponse, std::move(error)};
    }

    const auto parsed = ParseResponse(*transaction.response, address, masterId_);
    if (!parsed.ok) {
        MarkReadFailure(runtime, parsed.error);
        return DriverResult{
            address, operation, DriverOutcome::InvalidResponse, parsed.error};
    }
    if (parsed.state.command != Command::Read) {
        std::string error = "MDV read transaction expected a C0 response";
        MarkReadFailure(runtime, error);
        return DriverResult{
            address, operation, DriverOutcome::InvalidResponse, std::move(error)};
    }

    runtime.device.SynchronizeReadState(parsed.state);
    MarkReadSuccess(runtime);

    // A C0 response may still contain the old value after C3. Only if pending
    // fields remain do we queue another complete cached C3 frame.
    if (runtime.device.QueueSetCommandIfPending()) {
        EnqueueSet(runtime);
    }

    return DriverResult{address, operation, DriverOutcome::Success, {}};
}

DriverResult MdvDriver::ExecuteSet(DeviceRuntime& runtime)
{
    const auto address = runtime.device.Address();
    const auto snapshot = runtime.device.PrepareSetFrameForSend();
    const auto transaction = transport_.Execute(snapshot.frame);

    DriverResult result;
    result.address = address;
    result.operation = DriverOperation::SetState;

    if (transaction.status != TransactionStatus::Success) {
        result.outcome = ToDriverOutcome(transaction.status);
        result.error = transaction.error.empty()
            ? DefaultTransactionError(transaction.status)
            : transaction.error;
        MarkSetFailure(runtime, result.error);
    }
    else if (!transaction.response.has_value()) {
        result.outcome = DriverOutcome::InvalidResponse;
        result.error = "successful MDV transaction has no response frame";
        MarkSetFailure(runtime, result.error);
    }
    else {
        const auto parsed = ParseResponse(*transaction.response, address, masterId_);
        if (!parsed.ok) {
            result.outcome = DriverOutcome::InvalidResponse;
            result.error = parsed.error;
            MarkSetFailure(runtime, result.error);
        }
        else if (parsed.state.command != Command::Set) {
            result.outcome = DriverOutcome::InvalidResponse;
            result.error = "MDV set transaction expected a C3 response";
            MarkSetFailure(runtime, result.error);
        }
        else {
            // A C3 response is deliberately not copied into DeviceContext: it
            // may still contain the previous values.
            result.outcome = DriverOutcome::Success;
            MarkSetSuccess(runtime);
        }
    }

    runtime.device.FinishSetFrameSend(snapshot.revision);

    // Confirm even after timeout: the fan coil may have accepted C3 although
    // its response was lost. This prevents unsafe immediate duplicate writes.
    EnqueueConfirmation(runtime);

    // A newer command may have arrived while this immutable snapshot was sent.
    if (runtime.device.IsSetCommandQueued()) {
        EnqueueSet(runtime);
    }

    return result;
}

void MdvDriver::EnqueueSet(DeviceRuntime& runtime)
{
    if (!runtime.device.IsSetCommandQueued() || runtime.setQueueEntry) {
        return;
    }
    setQueue_.push_back(runtime.device.Address());
    runtime.setQueueEntry = true;
}

void MdvDriver::EnqueueConfirmation(DeviceRuntime& runtime)
{
    if (runtime.confirmQueueEntry) {
        return;
    }
    confirmationQueue_.push_back(runtime.device.Address());
    runtime.confirmQueueEntry = true;
}

DeviceRuntime& MdvDriver::PopSet()
{
    const auto address = setQueue_.front();
    setQueue_.pop_front();
    auto& runtime = DeviceByAddress(address);
    runtime.setQueueEntry = false;
    return runtime;
}

DeviceRuntime& MdvDriver::PopConfirmation()
{
    const auto address = confirmationQueue_.front();
    confirmationQueue_.pop_front();
    auto& runtime = DeviceByAddress(address);
    runtime.confirmQueueEntry = false;
    return runtime;
}

DeviceRuntime& MdvDriver::NextPollDevice() noexcept
{
    auto& runtime = devices_[nextPollIndex_];
    nextPollIndex_ = (nextPollIndex_ + 1) % devices_.size();
    return runtime;
}

void MdvDriver::MarkReadSuccess(DeviceRuntime& runtime) noexcept
{
    runtime.online = true;
    ++runtime.successfulReads;
    runtime.consecutiveReadFailures = 0;
    runtime.lastError.clear();
}

void MdvDriver::MarkReadFailure(DeviceRuntime& runtime, std::string error)
{
    runtime.online = false;
    ++runtime.failedReads;
    ++runtime.consecutiveReadFailures;
    runtime.lastError = std::move(error);
}

void MdvDriver::MarkSetSuccess(DeviceRuntime& runtime) noexcept
{
    runtime.online = true;
    ++runtime.successfulSets;
    runtime.lastError.clear();
}

void MdvDriver::MarkSetFailure(DeviceRuntime& runtime, std::string error)
{
    ++runtime.failedSets;
    runtime.lastError = std::move(error);
}

} // namespace mdv
