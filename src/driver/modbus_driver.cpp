#include "modbus_driver.h"

#include "modbus_resolver.h"
#include "modbus_rtu.h"
#include "modbus_runtime_profile.h"
#include "modbus_semantic.h"

#include <algorithm>
#include <array>
#include <cmath>
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

[[nodiscard]] std::optional<std::size_t> WritableControlIndex(
    DriverControl control) noexcept
{
    switch (control) {
    case DriverControl::Power:
        return 0U;
    case DriverControl::Mode:
        return 1U;
    case DriverControl::FanSpeed:
        return 2U;
    case DriverControl::SetTemperature:
        return 3U;
    case DriverControl::Blinds:
    case DriverControl::Blocked:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string_view> SemanticPointName(
    DriverControl control) noexcept
{
    switch (control) {
    case DriverControl::Power:
        return "power";
    case DriverControl::Mode:
        return "mode";
    case DriverControl::FanSpeed:
        return "fanSpeed";
    case DriverControl::SetTemperature:
        return "setTemperature";
    case DriverControl::Blinds:
    case DriverControl::Blocked:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] bool FactualValueMatches(
    const DriverDeviceState& state,
    DriverControl control,
    const DriverCommandValue& desired) noexcept
{
    switch (control) {
    case DriverControl::Power: {
        const auto* value = std::get_if<bool>(&desired);
        return value != nullptr && state.power == *value;
    }
    case DriverControl::Mode: {
        const auto* value = std::get_if<HvacMode>(&desired);
        return value != nullptr && state.mode.has_value() &&
            *state.mode == *value;
    }
    case DriverControl::FanSpeed: {
        const auto* value = std::get_if<HvacFanSpeed>(&desired);
        return value != nullptr && state.fanSpeed.has_value() &&
            *state.fanSpeed == *value;
    }
    case DriverControl::SetTemperature: {
        const auto* value = std::get_if<double>(&desired);
        return value != nullptr && state.setTemperature.has_value() &&
            std::fabs(*state.setTemperature - *value) < 0.000001;
    }
    case DriverControl::Blinds:
    case DriverControl::Blocked:
        return false;
    }
    return false;
}

} // namespace

ModbusDriver::ModbusDriver(
    std::vector<std::uint8_t> logicalAddresses,
    const ModbusProfile& profile,
    ITransactionTransport& transport,
    ModbusDriverPolicy policy)
    : profile_(profile),
      policy_(policy),
      transport_(transport)
{
    if (policy_.maxWriteAttempts == 0U ||
        policy_.maxWriteAttempts > 10U) {
        throw std::invalid_argument(
            "Modbus write attempts must be in range 1..10");
    }
    if (policy_.maxConfirmationAttempts == 0U ||
        policy_.maxConfirmationAttempts > 10U) {
        throw std::invalid_argument(
            "Modbus confirmation attempts must be in range 1..10");
    }
    if (policy_.maxPriorityOperationsBeforePoll == 0U ||
        policy_.maxPriorityOperationsBeforePoll > 64U) {
        throw std::invalid_argument(
            "Modbus priority burst must be in range 1..64");
    }
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
            .pendingWrites = {},
        });
    }
}

DriverResult ModbusDriver::ProcessNext()
{
    if (priorityOperations_ >= policy_.maxPriorityOperationsBeforePoll) {
        return ProcessPoll();
    }

    if (auto work = PopValidWork(confirmationQueue_);
        work.has_value()) {
        ++priorityOperations_;
        return ConfirmWrite(*work->runtime, work->control);
    }

    if (auto work = PopValidWork(writeQueue_);
        work.has_value()) {
        ++priorityOperations_;
        return ExecuteWrite(*work->runtime, work->control);
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

    const auto pointName = SemanticPointName(command.control);
    if (!pointName.has_value() ||
        !IsModbusRuntimeWritablePoint(profile_, *pointName)) {
        throw std::invalid_argument(
            "Modbus control is not writable in the selected profile");
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
            "profile does not resolve the Modbus write location for '" +
            std::string(*pointName) + "'");
    }
    if (location->space != RegisterSpace::HoldingRegister) {
        throw std::invalid_argument(
            "current Modbus runtime supports writes to holding_register only");
    }

    const auto profilePoint = profile_.points.find(*pointName);
    if (profilePoint == profile_.points.end() ||
        !profilePoint->second.read.has_value()) {
        throw std::invalid_argument(
            "profile has no readable confirmation point for '" +
            std::string(*pointName) + "'");
    }

    std::optional<ResolvedRegisterLocation> readLocation;
    try {
        readLocation = ResolveRegisterLocation(
            profile_,
            command.address,
            *profilePoint->second.read);
    }
    catch (const ResolverError& error) {
        throw std::invalid_argument(error.what());
    }
    if (!readLocation.has_value() ||
        readLocation->space != RegisterSpace::HoldingRegister) {
        throw std::invalid_argument(
            "profile does not resolve a readable holding-register "
            "confirmation point for '" + std::string(*pointName) + "'");
    }

    const auto revision = ++nextCommandRevision_;
    auto& pending = PendingByControl(runtime, command.control);

    // A newer command matching the last factual state cancels any older
    // pending command. Queue entries carry revisions and become harmlessly
    // stale, so no old write can escape after this point.
    if (FactualValueMatches(runtime.state, command.control, command.value)) {
        pending.reset();
        return;
    }

    pending = PendingWrite{
        .control = command.control,
        .desired = command.value,
        .pointName = std::string(*pointName),
        .rawValue = encoded.rawValue,
        .slaveId = location->slaveId,
        .writeAddress = location->address,
        .readLocation = *readLocation,
        .revision = revision,
        .writeAttempts = 0,
        .confirmationAttempts = 0,
    };
    EnqueueWrite(runtime, command.control);
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
            return std::any_of(
                runtime.pendingWrites.begin(),
                runtime.pendingWrites.end(),
                [](const auto& pending) {
                    return pending.has_value();
                });
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

std::optional<ModbusDriver::PendingWrite>& ModbusDriver::PendingByControl(
    DeviceRuntime& runtime,
    DriverControl control)
{
    const auto index = WritableControlIndex(control);
    if (!index.has_value()) {
        throw std::invalid_argument(
            "Modbus control has no confirmed-write state machine");
    }
    return runtime.pendingWrites[*index];
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

    std::vector<std::vector<std::uint16_t>> batchValues;
    batchValues.reserve(runtime.pollPlan.semanticBatches.size());
    for (const auto& batch : runtime.pollPlan.semanticBatches) {
        RawBatchReadResult read = ReadSemanticBatch(batch);
        if (!read.success) {
            return MarkOffline(
                runtime,
                DriverOperation::PollRead,
                read.outcome,
                std::move(read.error));
        }
        batchValues.push_back(std::move(read.values));
    }

    for (const auto& point : runtime.pollPlan.semanticReads) {
        if (point.batchIndex >= batchValues.size() ||
            point.registerOffset >= batchValues[point.batchIndex].size()) {
            return MarkOffline(
                runtime,
                DriverOperation::PollRead,
                DriverOutcome::InvalidResponse,
                "resolved Modbus semantic read is outside its batch");
        }

        try {
            ApplySemanticRead(
                snapshot,
                profile_,
                point.pointName,
                batchValues[point.batchIndex][point.registerOffset]);
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
    // a latest desired value, that command is complete and any queued work for
    // its revision becomes stale.
    for (auto& pending : runtime.pendingWrites) {
        if (pending.has_value() &&
            FactualValueMatches(
                runtime.state,
                pending->control,
                pending->desired)) {
            pending.reset();
        }
    }

    return DriverResult{
        .address = runtime.logicalAddress,
        .operation = DriverOperation::PollRead,
        .outcome = DriverOutcome::Success,
        .error = {},
    };
}

DriverResult ModbusDriver::ExecuteWrite(
    DeviceRuntime& runtime,
    DriverControl control)
{
    auto& pendingSlot = PendingByControl(runtime, control);
    if (!pendingSlot.has_value()) {
        return DriverResult{
            .address = runtime.logicalAddress,
            .operation = DriverOperation::SetState,
            .outcome = DriverOutcome::InvalidResponse,
            .error = "stale Modbus write work item",
        };
    }

    auto& pending = *pendingSlot;
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
        pendingSlot.reset();
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
            "Modbus '" + pending.pointName +
            "' write transport failure: " + error.what();
    }

    bool accepted = false;
    std::string error;
    if (transaction.status == TransactionStatus::Success) {
        if (!transaction.response.has_value()) {
            error = "successful Modbus '" + pending.pointName +
                "' write has no parsed response";
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
                error = "Modbus '" + pending.pointName +
                    "' write response does not match the request";
            }
        }
    }
    else {
        error = TransactionError(
            transaction,
            "Modbus write transaction failed");
    }

    if (accepted) {
        pending.confirmationAttempts = 0;
        EnqueueConfirmation(runtime, control);
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

    if (pending.writeAttempts < policy_.maxWriteAttempts) {
        EnqueueWrite(runtime, control);
    }
    else {
        pendingSlot.reset();
    }

    return DriverResult{
        .address = runtime.logicalAddress,
        .operation = DriverOperation::SetState,
        .outcome = outcome,
        .error = std::move(error),
    };
}

DriverResult ModbusDriver::ConfirmWrite(
    DeviceRuntime& runtime,
    DriverControl control)
{
    auto& pendingSlot = PendingByControl(runtime, control);
    if (!pendingSlot.has_value()) {
        return DriverResult{
            .address = runtime.logicalAddress,
            .operation = DriverOperation::ConfirmRead,
            .outcome = DriverOutcome::InvalidResponse,
            .error = "stale Modbus confirmation work item",
        };
    }

    auto& pending = *pendingSlot;
    ++pending.confirmationAttempts;

    const RawReadResult read = ReadSemanticRegister(pending.readLocation);
    if (!read.success) {
        runtime.state.online = false;

        if (pending.confirmationAttempts <
            policy_.maxConfirmationAttempts) {
            EnqueueConfirmation(runtime, control);
        }
        else {
            pendingSlot.reset();
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
            pending.pointName,
            read.value);
    }
    catch (const SemanticConversionError& error) {
        runtime.state.online = false;
        const std::string pointName = pending.pointName;
        pendingSlot.reset();
        return DriverResult{
            .address = runtime.logicalAddress,
            .operation = DriverOperation::ConfirmRead,
            .outcome = DriverOutcome::InvalidResponse,
            .error = "cannot decode Modbus '" + pointName +
                "' confirmation: " + error.what(),
        };
    }

    if (!FactualValueMatches(confirmed, control, pending.desired)) {
        const std::string error = "Modbus '" + pending.pointName +
            "' read-back does not match the requested value";

        // A valid but mismatching read proves that the device is reachable; it
        // must not publish a false offline state. Retry the write while budget
        // remains and report this as a failed SetState operation.
        runtime.state.online = true;
        if (pending.writeAttempts < policy_.maxWriteAttempts) {
            pending.confirmationAttempts = 0;
            EnqueueWrite(runtime, control);
        }
        else {
            pendingSlot.reset();
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
    pendingSlot.reset();

    return DriverResult{
        .address = runtime.logicalAddress,
        .operation = DriverOperation::ConfirmRead,
        .outcome = DriverOutcome::Success,
        .error = {},
    };
}

ModbusDriver::RawBatchReadResult ModbusDriver::ReadSemanticBatch(
    const ModbusSemanticReadBatch& batch)
{
    RtuAdu request;
    try {
        request = BuildReadHoldingRegistersRequest(
            batch.slaveId,
            batch.startAddress,
            batch.quantity);
    }
    catch (const std::exception& error) {
        return RawBatchReadResult{
            .outcome = DriverOutcome::InvalidResponse,
            .success = false,
            .values = {},
            .error = error.what(),
        };
    }

    TransactionResult transaction;
    try {
        transaction = transport_.Execute(request);
    }
    catch (const std::exception& error) {
        return RawBatchReadResult{
            .outcome = DriverOutcome::IoError,
            .success = false,
            .values = {},
            .error =
                std::string("Modbus transport failure: ") + error.what(),
        };
    }

    switch (transaction.status) {
    case TransactionStatus::Success: {
        if (!transaction.response.has_value()) {
            return RawBatchReadResult{
                .outcome = DriverOutcome::InvalidResponse,
                .success = false,
                .values = {},
                .error =
                    "successful Modbus transport result has no parsed response",
            };
        }

        const auto& response = *transaction.response;
        if (response.status != ResponseStatus::Success ||
            response.slaveId != batch.slaveId ||
            response.function != Function::ReadHoldingRegisters ||
            response.registers.size() != batch.quantity) {
            return RawBatchReadResult{
                .outcome = DriverOutcome::InvalidResponse,
                .success = false,
                .values = {},
                .error =
                    "Modbus semantic batch response does not match the request",
            };
        }

        return RawBatchReadResult{
            .outcome = DriverOutcome::Success,
            .success = true,
            .values = response.registers,
            .error = {},
        };
    }

    case TransactionStatus::Timeout:
        return RawBatchReadResult{
            .outcome = DriverOutcome::Timeout,
            .success = false,
            .values = {},
            .error = TransactionError(
                transaction,
                "Modbus semantic read timed out"),
        };

    case TransactionStatus::IoError:
        return RawBatchReadResult{
            .outcome = DriverOutcome::IoError,
            .success = false,
            .values = {},
            .error = TransactionError(
                transaction,
                "Modbus semantic read I/O error"),
        };

    case TransactionStatus::Exception:
        return RawBatchReadResult{
            .outcome = DriverOutcome::InvalidResponse,
            .success = false,
            .values = {},
            .error = TransactionError(
                transaction,
                "Modbus semantic read returned an exception"),
        };

    case TransactionStatus::InvalidRequest:
        return RawBatchReadResult{
            .outcome = DriverOutcome::InvalidResponse,
            .success = false,
            .values = {},
            .error = TransactionError(
                transaction,
                "Modbus semantic read request is invalid"),
        };

    case TransactionStatus::InvalidResponse:
        return RawBatchReadResult{
            .outcome = DriverOutcome::InvalidResponse,
            .success = false,
            .values = {},
            .error = TransactionError(
                transaction,
                "Modbus semantic read returned an invalid response"),
        };
    }

    return RawBatchReadResult{
        .outcome = DriverOutcome::InvalidResponse,
        .success = false,
        .values = {},
        .error = "unknown Modbus transaction status",
    };
}

ModbusDriver::RawReadResult ModbusDriver::ReadSemanticRegister(
    const ResolvedRegisterLocation& location)
{
    RawBatchReadResult batch = ReadSemanticBatch(ModbusSemanticReadBatch{
        .slaveId = location.slaveId,
        .startAddress = location.address,
        .quantity = 1U,
    });
    if (!batch.success) {
        return RawReadResult{
            .outcome = batch.outcome,
            .success = false,
            .value = 0,
            .error = std::move(batch.error),
        };
    }
    if (batch.values.size() != 1U) {
        return RawReadResult{
            .outcome = DriverOutcome::InvalidResponse,
            .success = false,
            .value = 0,
            .error = "single-register Modbus semantic read returned wrong size",
        };
    }
    return RawReadResult{
        .outcome = DriverOutcome::Success,
        .success = true,
        .value = batch.values.front(),
        .error = {},
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

void ModbusDriver::EnqueueWrite(
    const DeviceRuntime& runtime,
    DriverControl control)
{
    const auto index = WritableControlIndex(control);
    if (!index.has_value() ||
        !runtime.pendingWrites[*index].has_value()) {
        return;
    }
    writeQueue_.push_back(WorkItem{
        .logicalAddress = runtime.logicalAddress,
        .control = control,
        .revision = runtime.pendingWrites[*index]->revision,
    });
}

void ModbusDriver::EnqueueConfirmation(
    const DeviceRuntime& runtime,
    DriverControl control)
{
    const auto index = WritableControlIndex(control);
    if (!index.has_value() ||
        !runtime.pendingWrites[*index].has_value()) {
        return;
    }
    confirmationQueue_.push_back(WorkItem{
        .logicalAddress = runtime.logicalAddress,
        .control = control,
        .revision = runtime.pendingWrites[*index]->revision,
    });
}

std::optional<ModbusDriver::PendingWork> ModbusDriver::PopValidWork(
    std::deque<WorkItem>& queue)
{
    while (!queue.empty()) {
        const WorkItem item = queue.front();
        queue.pop_front();

        auto& runtime = DeviceByAddress(item.logicalAddress);
        const auto index = WritableControlIndex(item.control);
        if (index.has_value() &&
            runtime.pendingWrites[*index].has_value() &&
            runtime.pendingWrites[*index]->revision == item.revision) {
            return PendingWork{
                .runtime = &runtime,
                .control = item.control,
            };
        }
    }
    return std::nullopt;
}

DriverResult ModbusDriver::ProcessPoll()
{
    priorityOperations_ = 0;
    auto& runtime = devices_[nextPollIndex_];
    nextPollIndex_ = (nextPollIndex_ + 1U) % devices_.size();
    return Poll(runtime);
}

} // namespace mdv::modbus
