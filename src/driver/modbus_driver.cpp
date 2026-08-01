#include "modbus_driver.h"

#include "modbus_resolver.h"
#include "modbus_rtu.h"
#include "modbus_semantic.h"

#include <algorithm>
#include <array>
#include <exception>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace mdv::modbus {
namespace {

[[nodiscard]] std::string TransactionError(
    const TransactionResult& transaction,
    std::string_view fallback)
{
    return transaction.error.empty()
        ? std::string(fallback)
        : transaction.error;
}

[[nodiscard]] DriverOutcome ProbeFailureOutcome(
    const ScanResult& result) noexcept
{
    if (result.disposition == ScanDisposition::NotFound) {
        // The common MDVWB boundary already treats Timeout as the ordinary
        // per-device offline outcome. PresenceMismatch and probe exceptions
        // are therefore normalized to that same non-system-error category.
        return DriverOutcome::Timeout;
    }

    if (result.reason == ScanReason::IoError) {
        return DriverOutcome::IoError;
    }

    return DriverOutcome::InvalidResponse;
}

[[nodiscard]] std::string ProbeFailureMessage(const ScanResult& result)
{
    if (!result.diagnostic.empty()) {
        return result.diagnostic;
    }

    switch (result.reason) {
    case ScanReason::UnsupportedCandidate:
        return "profile does not support the configured logical address";
    case ScanReason::UnsupportedDataSpace:
        return "profile probe uses a Modbus data space unsupported by the runtime";
    case ScanReason::Timeout:
        return "Modbus probe timed out";
    case ScanReason::ExceptionResponse:
        return "Modbus probe returned an exception";
    case ScanReason::PresenceMismatch:
        return "Modbus probe response did not satisfy the profile presence rule";
    case ScanReason::InvalidResponse:
        return "Modbus probe returned an invalid response";
    case ScanReason::IoError:
        return "Modbus probe I/O error";
    case ScanReason::InvalidRequest:
        return "Modbus probe request is invalid";
    case ScanReason::Success:
        break;
    }
    return "Modbus probe failed";
}

[[nodiscard]] DriverOutcome TransactionOutcome(
    TransactionStatus status) noexcept
{
    switch (status) {
    case TransactionStatus::Success:
        return DriverOutcome::Success;
    case TransactionStatus::Timeout:
        return DriverOutcome::Timeout;
    case TransactionStatus::IoError:
        return DriverOutcome::IoError;
    case TransactionStatus::Exception:
    case TransactionStatus::InvalidRequest:
    case TransactionStatus::InvalidResponse:
        return DriverOutcome::InvalidResponse;
    }
    return DriverOutcome::InvalidResponse;
}

} // namespace

ModbusDriver::ModbusDriver(
    std::vector<std::uint8_t> logicalAddresses,
    const ModbusProfile& profile,
    ITransactionTransport& transport)
    : profile_(profile),
      transport_(transport)
{
    ModbusPollPlan plan = BuildModbusPollPlan(profile_, logicalAddresses);
    pollPlanMetrics_ = plan.metrics;
    devices_.reserve(plan.devices.size());

    for (auto& devicePlan : plan.devices) {
        DriverDeviceState state;
        state.address = devicePlan.logicalAddress;

        devices_.push_back(DeviceRuntime{
            .logicalAddress = devicePlan.logicalAddress,
            .pollPlan = std::move(devicePlan),
            .state = state,
            .pendingPower = std::nullopt,
        });
    }
}

DriverResult ModbusDriver::ProcessNext()
{
    if (priorityOperations_ >= kMaxModbusPriorityOperationsBeforePoll) {
        return ProcessPoll();
    }

    if (auto* runtime = PopValidWork(powerConfirmationQueue_);
        runtime != nullptr) {
        ++priorityOperations_;
        return ConfirmPowerWrite(*runtime);
    }

    if (auto* runtime = PopValidWork(powerWriteQueue_);
        runtime != nullptr) {
        ++priorityOperations_;
        return ExecutePowerWrite(*runtime);
    }

    return ProcessPoll();
}

void ModbusDriver::ApplyCommand(const DriverCommand& command)
{
    auto& runtime = DeviceByAddress(command.address);
    if (!runtime.state.online || !runtime.state.hasState) {
        throw std::logic_error(
            "Modbus device must have a current factual snapshot before commands");
    }

    if (command.control != DriverControl::Power) {
        throw std::invalid_argument(
            "current Modbus runtime supports Power commands only");
    }

    const auto* desired = std::get_if<bool>(&command.value);
    if (desired == nullptr) {
        throw std::invalid_argument(
            "Modbus Power command requires a boolean value");
    }

    EncodedSemanticWrite encoded;
    try {
        encoded = EncodeSemanticWrite(
            profile_,
            command.control,
            command.value);
    }
    catch (const SemanticConversionError& error) {
        throw std::invalid_argument(error.what());
    }

    std::optional<ResolvedRegisterLocation> location;
    try {
        location = ResolveRegisterLocation(
            profile_,
            command.address,
            encoded.location);
    }
    catch (const ResolverError& error) {
        throw std::invalid_argument(error.what());
    }

    if (!location.has_value()) {
        throw std::invalid_argument(
            "profile does not resolve the Modbus Power write location");
    }
    if (location->space != RegisterSpace::HoldingRegister) {
        throw std::invalid_argument(
            "current Modbus runtime supports Power writes to holding_register only");
    }

    const auto revision = ++nextCommandRevision_;

    // A newer command matching the last factual state cancels any older
    // pending command. Queue entries carry revisions and become harmlessly
    // stale, so no old write can escape after this point.
    if (runtime.state.power == *desired) {
        runtime.pendingPower.reset();
        return;
    }

    runtime.pendingPower = PendingPower{
        .desired = *desired,
        .rawValue = encoded.rawValue,
        .slaveId = location->slaveId,
        .writeAddress = location->address,
        .revision = revision,
        .writeAttempts = 0,
        .confirmationAttempts = 0,
    };
    EnqueuePowerWrite(runtime);
}

DriverDeviceState ModbusDriver::DeviceStateByAddress(
    std::uint8_t address) const
{
    return DeviceByAddress(address).state;
}

bool ModbusDriver::HasQueuedWork() const noexcept
{
    return std::any_of(
        devices_.begin(),
        devices_.end(),
        [](const DeviceRuntime& runtime) {
            return runtime.pendingPower.has_value();
        });
}

std::size_t ModbusDriver::DeviceCount() const noexcept
{
    return devices_.size();
}

std::uint8_t ModbusDriver::NextPollAddress() const noexcept
{
    return devices_[nextPollIndex_].logicalAddress;
}

const ModbusPollPlanMetrics& ModbusDriver::PollPlanMetrics() const noexcept
{
    return pollPlanMetrics_;
}

ModbusDriver::DeviceRuntime& ModbusDriver::DeviceByAddress(
    std::uint8_t address)
{
    for (auto& runtime : devices_) {
        if (runtime.logicalAddress == address) {
            return runtime;
        }
    }
    throw std::out_of_range(
        "Modbus logical address " + std::to_string(address) +
        " is not configured");
}

const ModbusDriver::DeviceRuntime& ModbusDriver::DeviceByAddress(
    std::uint8_t address) const
{
    for (const auto& runtime : devices_) {
        if (runtime.logicalAddress == address) {
            return runtime;
        }
    }
    throw std::out_of_range(
        "Modbus logical address " + std::to_string(address) +
        " is not configured");
}

DriverResult ModbusDriver::Poll(DeviceRuntime& runtime)
{
    ScanResult probe;
    try {
        probe = ExecuteScanProbe(runtime.pollPlan.probe, transport_);
    }
    catch (const std::exception& error) {
        return MarkOffline(
            runtime,
            DriverOperation::PollRead,
            DriverOutcome::IoError,
            std::string("Modbus probe transport failure: ") + error.what());
    }

    if (probe.disposition != ScanDisposition::Found) {
        return MarkOffline(
            runtime,
            DriverOperation::PollRead,
            ProbeFailureOutcome(probe),
            ProbeFailureMessage(probe));
    }

    DriverDeviceState snapshot;
    snapshot.address = runtime.logicalAddress;

    for (const auto& point : runtime.pollPlan.semanticReads) {
        const RawReadResult read = ReadSemanticRegister(point.location);
        if (!read.success) {
            return MarkOffline(
                runtime,
                DriverOperation::PollRead,
                read.outcome,
                read.error);
        }

        try {
            ApplySemanticRead(
                snapshot,
                profile_,
                point.pointName,
                read.value);
        }
        catch (const SemanticConversionError& error) {
            return MarkOffline(
                runtime,
                DriverOperation::PollRead,
                DriverOutcome::InvalidResponse,
                "cannot decode semantic point '" + point.pointName +
                    "': " + error.what());
        }
    }

    snapshot.online = true;
    snapshot.hasState = true;
    runtime.state = std::move(snapshot);

    // A bounded ordinary poll is also a valid factual read-back. If it observes
    // the latest desired Power value, the pending command is complete and any
    // queued confirmation for that revision becomes stale.
    if (runtime.pendingPower.has_value() &&
        runtime.state.power == runtime.pendingPower->desired) {
        runtime.pendingPower.reset();
    }

    return DriverResult{
        .address = runtime.logicalAddress,
        .operation = DriverOperation::PollRead,
        .outcome = DriverOutcome::Success,
        .error = {},
    };
}

DriverResult ModbusDriver::ExecutePowerWrite(DeviceRuntime& runtime)
{
    if (!runtime.pendingPower.has_value()) {
        return DriverResult{
            .address = runtime.logicalAddress,
            .operation = DriverOperation::SetState,
            .outcome = DriverOutcome::InvalidResponse,
            .error = "stale Modbus Power write work item",
        };
    }

    auto& pending = *runtime.pendingPower;
    ++pending.writeAttempts;

    RtuAdu request;
    try {
        const std::array<std::uint16_t, 1> values{pending.rawValue};
        request = BuildWriteMultipleRegistersRequest(
            pending.slaveId,
            pending.writeAddress,
            std::span<const std::uint16_t>(values));
    }
    catch (const std::exception& error) {
        runtime.pendingPower.reset();
        return DriverResult{
            .address = runtime.logicalAddress,
            .operation = DriverOperation::SetState,
            .outcome = DriverOutcome::InvalidResponse,
            .error = error.what(),
        };
    }

    TransactionResult transaction;
    try {
        transaction = transport_.Execute(request);
    }
    catch (const std::exception& error) {
        transaction.status = TransactionStatus::IoError;
        transaction.error =
            std::string("Modbus Power write transport failure: ") +
            error.what();
    }

    bool accepted = false;
    std::string error;
    if (transaction.status == TransactionStatus::Success) {
        if (!transaction.response.has_value()) {
            error =
                "successful Modbus Power write has no parsed response";
        }
        else {
            const auto& response = *transaction.response;
            accepted =
                response.status == ResponseStatus::Success &&
                response.slaveId == pending.slaveId &&
                response.function == Function::WriteMultipleRegisters &&
                response.startAddress.has_value() &&
                *response.startAddress == pending.writeAddress &&
                response.quantity.has_value() &&
                *response.quantity == 1U;
            if (!accepted) {
                error =
                    "Modbus Power write response does not match the request";
            }
        }
    }
    else {
        error = TransactionError(
            transaction,
            "Modbus Power write transaction failed");
    }

    if (accepted) {
        pending.confirmationAttempts = 0;
        EnqueuePowerConfirmation(runtime);
        return DriverResult{
            .address = runtime.logicalAddress,
            .operation = DriverOperation::SetState,
            .outcome = DriverOutcome::Success,
            .error = {},
        };
    }

    const DriverOutcome outcome =
        transaction.status == TransactionStatus::Success
            ? DriverOutcome::InvalidResponse
            : TransactionOutcome(transaction.status);

    if (pending.writeAttempts < kMaxModbusWriteAttempts) {
        EnqueuePowerWrite(runtime);
    }
    else {
        runtime.pendingPower.reset();
    }

    return DriverResult{
        .address = runtime.logicalAddress,
        .operation = DriverOperation::SetState,
        .outcome = outcome,
        .error = std::move(error),
    };
}

DriverResult ModbusDriver::ConfirmPowerWrite(DeviceRuntime& runtime)
{
    if (!runtime.pendingPower.has_value()) {
        return DriverResult{
            .address = runtime.logicalAddress,
            .operation = DriverOperation::ConfirmRead,
            .outcome = DriverOutcome::InvalidResponse,
            .error = "stale Modbus Power confirmation work item",
        };
    }

    auto& pending = *runtime.pendingPower;
    ++pending.confirmationAttempts;

    if (!runtime.pollPlan.powerRead.has_value()) {
        runtime.pendingPower.reset();
        return DriverResult{
            .address = runtime.logicalAddress,
            .operation = DriverOperation::ConfirmRead,
            .outcome = DriverOutcome::InvalidResponse,
            .error = "profile has no readable Power confirmation point",
        };
    }

    const RawReadResult read = ReadSemanticRegister(
        *runtime.pollPlan.powerRead);
    if (!read.success) {
        runtime.state.online = false;

        if (pending.confirmationAttempts <
            kMaxModbusConfirmationAttempts) {
            EnqueuePowerConfirmation(runtime);
        }
        else {
            runtime.pendingPower.reset();
        }

        return DriverResult{
            .address = runtime.logicalAddress,
            .operation = DriverOperation::ConfirmRead,
            .outcome = read.outcome,
            .error = read.error,
        };
    }

    DriverDeviceState confirmed = runtime.state;
    try {
        ApplySemanticRead(
            confirmed,
            profile_,
            "power",
            read.value);
    }
    catch (const SemanticConversionError& error) {
        runtime.pendingPower.reset();
        return DriverResult{
            .address = runtime.logicalAddress,
            .operation = DriverOperation::ConfirmRead,
            .outcome = DriverOutcome::InvalidResponse,
            .error =
                std::string("cannot decode Modbus Power confirmation: ") +
                error.what(),
        };
    }

    if (confirmed.power != pending.desired) {
        const std::string error =
            "Modbus Power read-back does not match the requested value";

        // A valid but mismatching read proves that the device is reachable; it
        // must not publish a false offline state. Retry the write while budget
        // remains and report this as a failed SetState operation.
        runtime.state.online = true;
        if (pending.writeAttempts < kMaxModbusWriteAttempts) {
            pending.confirmationAttempts = 0;
            EnqueuePowerWrite(runtime);
        }
        else {
            runtime.pendingPower.reset();
        }

        return DriverResult{
            .address = runtime.logicalAddress,
            .operation = DriverOperation::SetState,
            .outcome = DriverOutcome::InvalidResponse,
            .error = error,
        };
    }

    confirmed.online = true;
    confirmed.hasState = true;
    runtime.state = std::move(confirmed);
    runtime.pendingPower.reset();

    return DriverResult{
        .address = runtime.logicalAddress,
        .operation = DriverOperation::ConfirmRead,
        .outcome = DriverOutcome::Success,
        .error = {},
    };
}

ModbusDriver::RawReadResult ModbusDriver::ReadSemanticRegister(
    const ResolvedRegisterLocation& location)
{
    RtuAdu request;
    try {
        request = BuildReadHoldingRegistersRequest(
            location.slaveId,
            location.address,
            1U);
    }
    catch (const std::exception& error) {
        return RawReadResult{
            .outcome = DriverOutcome::InvalidResponse,
            .success = false,
            .value = 0,
            .error = error.what(),
        };
    }

    TransactionResult transaction;
    try {
        transaction = transport_.Execute(request);
    }
    catch (const std::exception& error) {
        return RawReadResult{
            .outcome = DriverOutcome::IoError,
            .success = false,
            .value = 0,
            .error =
                std::string("Modbus transport failure: ") + error.what(),
        };
    }

    switch (transaction.status) {
    case TransactionStatus::Success: {
        if (!transaction.response.has_value()) {
            return RawReadResult{
                .outcome = DriverOutcome::InvalidResponse,
                .success = false,
                .value = 0,
                .error =
                    "successful Modbus transport result has no parsed response",
            };
        }

        const auto& response = *transaction.response;
        if (response.status != ResponseStatus::Success ||
            response.slaveId != location.slaveId ||
            response.function != Function::ReadHoldingRegisters ||
            response.registers.size() != 1U) {
            return RawReadResult{
                .outcome = DriverOutcome::InvalidResponse,
                .success = false,
                .value = 0,
                .error =
                    "Modbus semantic read response does not match the request",
            };
        }

        return RawReadResult{
            .outcome = DriverOutcome::Success,
            .success = true,
            .value = response.registers.front(),
            .error = {},
        };
    }

    case TransactionStatus::Timeout:
        return RawReadResult{
            .outcome = DriverOutcome::Timeout,
            .success = false,
            .value = 0,
            .error = TransactionError(
                transaction,
                "Modbus semantic read timed out"),
        };

    case TransactionStatus::IoError:
        return RawReadResult{
            .outcome = DriverOutcome::IoError,
            .success = false,
            .value = 0,
            .error = TransactionError(
                transaction,
                "Modbus semantic read I/O error"),
        };

    case TransactionStatus::Exception:
        return RawReadResult{
            .outcome = DriverOutcome::InvalidResponse,
            .success = false,
            .value = 0,
            .error = TransactionError(
                transaction,
                "Modbus semantic read returned an exception"),
        };

    case TransactionStatus::InvalidRequest:
        return RawReadResult{
            .outcome = DriverOutcome::InvalidResponse,
            .success = false,
            .value = 0,
            .error = TransactionError(
                transaction,
                "Modbus semantic read request is invalid"),
        };

    case TransactionStatus::InvalidResponse:
        return RawReadResult{
            .outcome = DriverOutcome::InvalidResponse,
            .success = false,
            .value = 0,
            .error = TransactionError(
                transaction,
                "Modbus semantic read returned an invalid response"),
        };
    }

    return RawReadResult{
        .outcome = DriverOutcome::InvalidResponse,
        .success = false,
        .value = 0,
        .error = "unknown Modbus transaction status",
    };
}

DriverResult ModbusDriver::MarkOffline(
    DeviceRuntime& runtime,
    DriverOperation operation,
    DriverOutcome outcome,
    std::string error)
{
    runtime.state.online = false;
    return DriverResult{
        .address = runtime.logicalAddress,
        .operation = operation,
        .outcome = outcome,
        .error = std::move(error),
    };
}

void ModbusDriver::EnqueuePowerWrite(const DeviceRuntime& runtime)
{
    if (!runtime.pendingPower.has_value()) {
        return;
    }
    powerWriteQueue_.push_back(WorkItem{
        .logicalAddress = runtime.logicalAddress,
        .revision = runtime.pendingPower->revision,
    });
}

void ModbusDriver::EnqueuePowerConfirmation(const DeviceRuntime& runtime)
{
    if (!runtime.pendingPower.has_value()) {
        return;
    }
    powerConfirmationQueue_.push_back(WorkItem{
        .logicalAddress = runtime.logicalAddress,
        .revision = runtime.pendingPower->revision,
    });
}

ModbusDriver::DeviceRuntime* ModbusDriver::PopValidWork(
    std::deque<WorkItem>& queue)
{
    while (!queue.empty()) {
        const WorkItem item = queue.front();
        queue.pop_front();

        auto& runtime = DeviceByAddress(item.logicalAddress);
        if (runtime.pendingPower.has_value() &&
            runtime.pendingPower->revision == item.revision) {
            return &runtime;
        }
    }
    return nullptr;
}

DriverResult ModbusDriver::ProcessPoll()
{
    priorityOperations_ = 0;
    auto& runtime = devices_[nextPollIndex_];
    nextPollIndex_ = (nextPollIndex_ + 1U) % devices_.size();
    return Poll(runtime);
}

} // namespace mdv::modbus
