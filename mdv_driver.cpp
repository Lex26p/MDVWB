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

[[nodiscard]] PollOutcome ToPollOutcome(TransactionStatus status) noexcept
{
    switch (status) {
    case TransactionStatus::Success:
        return PollOutcome::Success;
    case TransactionStatus::Timeout:
        return PollOutcome::Timeout;
    case TransactionStatus::IoError:
        return PollOutcome::IoError;
    }
    return PollOutcome::IoError;
}

} // namespace

MdvPollingDriver::MdvPollingDriver(
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

PollResult MdvPollingDriver::PollNext()
{
    auto& runtime = devices_[nextIndex_];
    nextIndex_ = (nextIndex_ + 1) % devices_.size();

    const auto address = runtime.device.Address();
    const auto transaction = transport_.Execute(BuildReadRequest(address, masterId_));

    if (transaction.status != TransactionStatus::Success) {
        auto error = transaction.error;
        if (error.empty()) {
            error = transaction.status == TransactionStatus::Timeout
                ? "MDV response timeout"
                : "MDV serial I/O error";
        }
        MarkFailure(runtime, error);
        return PollResult{address, ToPollOutcome(transaction.status), std::move(error)};
    }

    if (!transaction.response.has_value()) {
        std::string error = "successful MDV transaction has no response frame";
        MarkFailure(runtime, error);
        return PollResult{address, PollOutcome::InvalidResponse, std::move(error)};
    }

    const auto parsed = ParseResponse(*transaction.response, address, masterId_);
    if (!parsed.ok) {
        MarkFailure(runtime, parsed.error);
        return PollResult{address, PollOutcome::InvalidResponse, parsed.error};
    }
    if (parsed.state.command != Command::Read) {
        std::string error = "MDV polling expected a C0 response";
        MarkFailure(runtime, error);
        return PollResult{address, PollOutcome::InvalidResponse, std::move(error)};
    }

    runtime.device.SynchronizeReadState(parsed.state);
    MarkSuccess(runtime);
    return PollResult{address, PollOutcome::Success, {}};
}

std::size_t MdvPollingDriver::DeviceCount() const noexcept
{
    return devices_.size();
}

std::uint8_t MdvPollingDriver::NextAddress() const noexcept
{
    return devices_[nextIndex_].device.Address();
}

DeviceRuntime& MdvPollingDriver::DeviceByAddress(std::uint8_t address)
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

const DeviceRuntime& MdvPollingDriver::DeviceByAddress(std::uint8_t address) const
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

void MdvPollingDriver::MarkSuccess(DeviceRuntime& runtime) noexcept
{
    runtime.online = true;
    ++runtime.successfulPolls;
    runtime.consecutiveFailures = 0;
    runtime.lastError.clear();
}

void MdvPollingDriver::MarkFailure(DeviceRuntime& runtime, std::string error)
{
    runtime.online = false;
    ++runtime.failedPolls;
    ++runtime.consecutiveFailures;
    runtime.lastError = std::move(error);
}

} // namespace mdv
