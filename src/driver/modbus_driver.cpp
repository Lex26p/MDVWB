#include "modbus_driver.h"
#include "modbus_write_block.h"

#include "modbus_resolver.h"
#include "modbus_rtu.h"
#include "modbus_runtime_profile.h"
#include "modbus_semantic.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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

[[nodiscard]] std::string ReadFailureContext(
    std::string_view stage,
    std::uint8_t logicalAddress,
    std::uint8_t slaveId,
    std::uint16_t startAddress,
    std::uint16_t quantity,
    std::string_view detail)
{
    std::string result =
        "stage=" + std::string(stage) +
        ", logical-address=" + std::to_string(logicalAddress) +
        ", slave-id=" + std::to_string(slaveId);

    if (quantity <= 1U) {
        result += ", register=" + std::to_string(startAddress);
    }
    else {
        const auto lastAddress =
            static_cast<std::uint32_t>(startAddress) + quantity - 1U;
        result += ", registers=" + std::to_string(startAddress) + ".." +
            std::to_string(lastAddress);
    }

    result += ": ";
    result += detail;
    return result;
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
            .consecutivePollFailures = 0,
        });
    }
}

DriverResult ModbusDriver::ProcessNext()
{
    for (auto& runtime : devices_) {
        for (auto& pending : runtime.pendingWrites) {
            if (pending && pending->confirmationDeadline &&
                std::chrono::steady_clock::now() >= *pending->confirmationDeadline) {
                const auto name = pending->pointName;
                pending.reset();
                return DriverResult{
                    .address = runtime.logicalAddress, .operation = DriverOperation::SetState,
                    .outcome = DriverOutcome::Timeout,
                    .error = "gateway factual confirmation timed out for '" + name + "'",
                };
            }
        }
    }
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
        !IsReadableSpace(readLocation->space)) {
        throw std::invalid_argument(
            "profile does not resolve a readable "
            "confirmation point for '" + std::string(*pointName) + "'");
    }

    const auto revision = ++nextCommandRevision_;
    auto& pending = PendingByControl(runtime, command.control);

    bool supersedesSent = pending && pending->writeAttempts != 0;
    if (profile_.writeBlock) {
        // Mode includes power-on in shared-register protocols. A following
        // Power=on must not discard the requested mode; Power=off supersedes it.
        for (auto& other : runtime.pendingWrites) {
            if (other && other->writeAddress == location->address) {
                const bool keepMode = command.control == DriverControl::Power &&
                    std::get<bool>(command.value) && other->control == DriverControl::Mode;
                if (keepMode) continue;
                supersedesSent = supersedesSent || other->writeAttempts != 0;
                other.reset();
            }
        }
    }

    // A newer command matching the last factual state cancels any older
    // pending command. Queue entries carry revisions and become harmlessly
    // stale, so no old write can escape after this point.
    if ((!profile_.writeBlock || !supersedesSent) &&
        FactualValueMatches(runtime.state, command.control, command.value)) {
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
        .writeFunction = encoded.location.writeFunction,
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
        return RecordPollFailure(
            runtime,
            DriverOutcome::IoError,
            ReadFailureContext(
                "probe",
                runtime.logicalAddress,
                runtime.pollPlan.probe.slaveId,
                runtime.pollPlan.probe.address,
                runtime.pollPlan.probe.quantity,
                std::string("Modbus probe transport failure: ") +
                    error.what()));
    }

    if (probe.disposition != ScanDisposition::Found) {
        return RecordPollFailure(
            runtime,
            ProbeFailureOutcome(probe),
            ReadFailureContext(
                "probe",
                runtime.logicalAddress,
                runtime.pollPlan.probe.slaveId,
                runtime.pollPlan.probe.address,
                runtime.pollPlan.probe.quantity,
                ProbeFailureMessage(probe)));
    }

    DriverDeviceState snapshot;
    snapshot.address = runtime.logicalAddress;

    std::vector<std::vector<std::uint16_t>> batchValues;
    batchValues.reserve(runtime.pollPlan.semanticBatches.size());
    for (const auto& batch : runtime.pollPlan.semanticBatches) {
        RawBatchReadResult read = ReadSemanticBatch(batch);
        if (!read.success) {
            return RecordPollFailure(
                runtime,
                read.outcome,
                ReadFailureContext(
                    "semantic-read",
                    runtime.logicalAddress,
                    batch.slaveId,
                    batch.startAddress,
                    batch.quantity,
                    read.error));
        }
        batchValues.push_back(std::move(read.values));
    }

    for (const auto& point : runtime.pollPlan.semanticReads) {
        std::optional<std::uint16_t> rawValue;
        if (point.probeRegisterOffset.has_value()) {
            if (*point.probeRegisterOffset < probe.registers.size()) {
                rawValue = probe.registers[*point.probeRegisterOffset];
            }
        }
        else if (point.batchIndex < batchValues.size() &&
                 point.registerOffset < batchValues[point.batchIndex].size()) {
            rawValue = batchValues[point.batchIndex][point.registerOffset];
        }

        if (!rawValue.has_value()) {
            return RecordPollFailure(
                runtime,
                DriverOutcome::InvalidResponse,
                ReadFailureContext(
                    "semantic-decode",
                    runtime.logicalAddress,
                    point.location.slaveId,
                    point.location.address,
                    1U,
                    point.probeRegisterOffset.has_value()
                        ? "resolved Modbus semantic read is outside the probe response"
                        : "resolved Modbus semantic read is outside its batch"));
        }

        try {
            ApplySemanticRead(
                snapshot,
                profile_,
                point.pointName,
                *rawValue);
        }
        catch (const SemanticConversionError& error) {
            return RecordPollFailure(
                runtime,
                DriverOutcome::InvalidResponse,
                ReadFailureContext(
                    "semantic-decode",
                    runtime.logicalAddress,
                    point.location.slaveId,
                    point.location.address,
                    1U,
                    "cannot decode semantic point '" + point.pointName +
                        "': " + error.what()));
        }
    }

    snapshot.online = true;
    snapshot.hasState = true;
    runtime.state = std::move(snapshot);
    runtime.consecutivePollFailures = 0;

    // A bounded ordinary poll is also a valid factual read-back. If it observes
    // a latest desired value, that command is complete and any queued work for
    // its revision becomes stale.
    for (auto& pending : runtime.pendingWrites) {
        if (pending.has_value() &&
            (profile_.confirmationTimeoutMs == 0 || pending->confirmationDeadline.has_value()) &&
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
    std::uint16_t requestAddress = pending.writeAddress;
    std::uint16_t requestQuantity = 1;
    Function expectedFunction = Function::WriteMultipleRegisters;
    try {
        if (profile_.writeBlock) {
            const auto online = ExecuteScanProbe(runtime.pollPlan.probe, transport_);
            if (online.disposition != ScanDisposition::Found) {
                throw std::invalid_argument("writeBlock device presence is not confirmed");
            }
            const auto& block = *profile_.writeBlock;
            const auto source = ResolveRegisterLocation(profile_, runtime.logicalAddress, block.snapshot);
            const auto target = ResolveRegisterLocation(profile_, runtime.logicalAddress, block.write);
            if (!source || !target) throw std::invalid_argument("cannot resolve writeBlock");
            const auto snapshot = ReadSemanticBatch(ModbusSemanticReadBatch{
                .slaveId = source->slaveId, .startAddress = source->address,
                .quantity = block.snapshotQuantity, .space = source->space,
            });
            if (!snapshot.success) throw std::invalid_argument("writeBlock snapshot: " + snapshot.error);
            std::map<std::size_t, std::uint16_t> overrides;
            for (const auto& other : runtime.pendingWrites) {
                if (other) overrides[other->writeAddress - target->address] = other->rawValue;
            }
            // Preserve factual power even when an off unit still reports its
            // previous mode. Replaying that mode alone would switch it on.
            const auto powerPoint = profile_.points.find("power");
            if (profile_.capabilities.power && powerPoint != profile_.points.end() &&
                powerPoint->second.read && powerPoint->second.write) {
                const auto powerWrite = ResolveRegisterLocation(profile_, runtime.logicalAddress, *powerPoint->second.write);
                const auto powerRead = ResolveRegisterLocation(profile_, runtime.logicalAddress, *powerPoint->second.read);
                if (!powerWrite || !powerRead) throw std::invalid_argument("cannot resolve writeBlock power");
                const auto offset = static_cast<std::size_t>(powerWrite->address - target->address);
                if (!overrides.contains(offset)) {
                    const auto read = ReadSemanticRegister(*powerRead);
                    if (!read.success) throw std::invalid_argument("writeBlock power snapshot: " + read.error);
                    DriverDeviceState factual;
                    ApplySemanticRead(factual, profile_, "power", read.value);
                    if (!factual.power) {
                        overrides[offset] = EncodeSemanticWrite(profile_, DriverControl::Power, false).rawValue;
                    }
                }
            }
            const auto values = EncodeWriteBlockSnapshot(block, snapshot.values, overrides);
            requestAddress = target->address;
            requestQuantity = static_cast<std::uint16_t>(values.size());
            request = BuildWriteMultipleRegistersRequest(target->slaveId, requestAddress, values);
        }
        else if (pending.writeFunction == WriteFunction::WriteSingleRegister) {
            expectedFunction = Function::WriteSingleRegister;
            request = BuildWriteSingleRegisterRequest(
                pending.slaveId,
                pending.writeAddress,
                pending.rawValue);
        }
        else {
            const std::array<std::uint16_t, 1> values{pending.rawValue};
            request = BuildWriteMultipleRegistersRequest(
                pending.slaveId,
                pending.writeAddress,
                std::span<const std::uint16_t>(values));
        }
    }
    catch (const std::exception& error) {
        const auto slaveId = pending.slaveId;
        const auto writeAddress = pending.writeAddress;
        pendingSlot.reset();
        return DriverResult{
            .address = runtime.logicalAddress,
            .operation = DriverOperation::SetState,
            .outcome = DriverOutcome::InvalidResponse,
            .error = ReadFailureContext(
                "write-build",
                runtime.logicalAddress,
                slaveId,
                writeAddress,
                1U,
                error.what()),
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
                response.function == expectedFunction &&
                response.startAddress.has_value() &&
                *response.startAddress == requestAddress;
            if (accepted &&
                expectedFunction == Function::WriteSingleRegister) {
                accepted = response.value.has_value() &&
                    *response.value == pending.rawValue;
            }
            else if (accepted) {
                accepted = response.quantity.has_value() &&
                    *response.quantity == requestQuantity;
            }
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
        if (profile_.confirmationTimeoutMs != 0) {
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(profile_.confirmationTimeoutMs);
            if (profile_.writeBlock) {
                // Every latest pending field was included in this one FC16 packet.
                for (auto& other : runtime.pendingWrites) {
                    if (!other) continue;
                    if (!other->confirmationDeadline) other->confirmationDeadline = deadline;
                    other->writeAttempts = std::max(1U, other->writeAttempts);
                    other->revision = ++nextCommandRevision_; // invalidate queued duplicate sends
                }
            }
            else pending.confirmationDeadline = deadline;
        }
        else EnqueueConfirmation(runtime, control);
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
    const auto slaveId = pending.slaveId;
    const auto writeAddress = pending.writeAddress;

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
        .error = ReadFailureContext(
            "write",
            runtime.logicalAddress,
            slaveId,
            writeAddress,
            1U,
            error),
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
        const auto readLocation = pending.readLocation;
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
            .error = ReadFailureContext(
                "confirmation-read",
                runtime.logicalAddress,
                readLocation.slaveId,
                readLocation.address,
                1U,
                read.error),
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
        const std::string pointName = pending.pointName;
        const auto readLocation = pending.readLocation;
        pendingSlot.reset();
        return DriverResult{
            .address = runtime.logicalAddress,
            .operation = DriverOperation::ConfirmRead,
            .outcome = DriverOutcome::InvalidResponse,
            .error = ReadFailureContext(
                "confirmation-decode",
                runtime.logicalAddress,
                readLocation.slaveId,
                readLocation.address,
                1U,
                "cannot decode Modbus '" + pointName +
                    "' confirmation: " + error.what()),
        };
    }

    if (!FactualValueMatches(confirmed, control, pending.desired)) {
        const std::string error = ReadFailureContext(
            "confirmation-compare",
            runtime.logicalAddress,
            pending.readLocation.slaveId,
            pending.readLocation.address,
            1U,
            "Modbus '" + pending.pointName +
                "' read-back does not match the requested value");

        // A valid but mismatching confirmation must not change availability;
        // only a complete ordinary poll owns offline/recovery transitions.
        // Retry the write while budget remains and report a failed SetState.
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
        request = BuildReadRequest(ReadFunction(batch.space),
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

        auto response = *transaction.response;
        if (batch.space == RegisterSpace::DiscreteInput &&
            response.registers.size() == ((batch.quantity + 7U) / 8U) * 8U) {
            response.registers.resize(batch.quantity);
        }
        if (response.status != ResponseStatus::Success ||
            response.slaveId != batch.slaveId ||
            response.function != ReadFunction(batch.space) ||
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
        .space = location.space,
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

DriverResult ModbusDriver::RecordPollFailure(
    DeviceRuntime& runtime,
    DriverOutcome outcome,
    std::string error)
{
    if (runtime.consecutivePollFailures <
        kModbusPollFailuresBeforeOffline) {
        ++runtime.consecutivePollFailures;
    }

    const bool thresholdReached =
        runtime.consecutivePollFailures >=
        kModbusPollFailuresBeforeOffline;
    if (thresholdReached) {
        runtime.state.online = false;
    }

    error += "; consecutive-poll-failures=" +
        std::to_string(runtime.consecutivePollFailures) + "/" +
        std::to_string(kModbusPollFailuresBeforeOffline);
    error += thresholdReached
        ? "; device marked offline"
        : "; previous availability preserved";

    return DriverResult{
        .address = runtime.logicalAddress,
        .operation = DriverOperation::PollRead,
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
