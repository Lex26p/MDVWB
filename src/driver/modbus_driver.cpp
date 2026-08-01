#include "modbus_driver.h"

#include "modbus_resolver.h"
#include "modbus_rtu.h"
#include "modbus_semantic.h"

#include <array>
#include <exception>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mdv::modbus {
namespace {

constexpr std::array<std::string_view, 8> kSemanticPointNames{
    "power",
    "mode",
    "fanSpeed",
    "setTemperature",
    "roomTemperature",
    "alarmCode",
    "blinds",
    "blocked",
};

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

} // namespace

ModbusDriver::ModbusDriver(
    std::vector<std::uint8_t> logicalAddresses,
    const ModbusProfile& profile,
    ITransactionTransport& transport)
    : profile_(profile),
      transport_(transport)
{
    if (logicalAddresses.empty()) {
        throw std::invalid_argument(
            "Modbus driver requires at least one logical address");
    }

    ScanPlan plan;
    try {
        plan = BuildScanPlan(profile_);
    }
    catch (const ScanPlanError& error) {
        throw std::invalid_argument(
            "cannot build Modbus polling plan for profile '" +
            profile_.id + "': " + error.what());
    }

    std::set<std::uint8_t> unique;
    devices_.reserve(logicalAddresses.size());

    for (const auto logicalAddress : logicalAddresses) {
        if (logicalAddress < kMinLogicalAddress ||
            logicalAddress > kMaxLogicalAddress) {
            throw std::invalid_argument(
                "Modbus logical address must be in range 1..63");
        }
        if (!unique.insert(logicalAddress).second) {
            throw std::invalid_argument(
                "duplicate Modbus logical address " +
                std::to_string(logicalAddress));
        }

        const auto& candidate =
            plan[static_cast<std::size_t>(logicalAddress - 1U)];
        if (!candidate.probe.has_value()) {
            throw std::invalid_argument(
                "profile '" + profile_.id +
                "' does not support configured logical address " +
                std::to_string(logicalAddress));
        }

        ValidateReadablePoints(logicalAddress);

        DriverDeviceState state;
        state.address = logicalAddress;

        devices_.push_back(DeviceRuntime{
            .logicalAddress = logicalAddress,
            .probe = *candidate.probe,
            .state = state,
        });
    }
}

DriverResult ModbusDriver::ProcessNext()
{
    auto& runtime = devices_[nextPollIndex_];
    nextPollIndex_ = (nextPollIndex_ + 1U) % devices_.size();
    return Poll(runtime);
}

void ModbusDriver::ApplyCommand(const DriverCommand& command)
{
    static_cast<void>(DeviceByAddress(command.address));
    throw std::logic_error(
        "Modbus writes are not enabled in the read-only driver stage");
}

DriverDeviceState ModbusDriver::DeviceStateByAddress(
    std::uint8_t address) const
{
    return DeviceByAddress(address).state;
}

bool ModbusDriver::HasQueuedWork() const noexcept
{
    return false;
}

std::size_t ModbusDriver::DeviceCount() const noexcept
{
    return devices_.size();
}

std::uint8_t ModbusDriver::NextPollAddress() const noexcept
{
    return devices_[nextPollIndex_].logicalAddress;
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
        probe = ExecuteScanProbe(runtime.probe, transport_);
    }
    catch (const std::exception& error) {
        return MarkOffline(
            runtime,
            DriverOutcome::IoError,
            std::string("Modbus probe transport failure: ") + error.what());
    }

    if (probe.disposition != ScanDisposition::Found) {
        return MarkOffline(
            runtime,
            ProbeFailureOutcome(probe),
            ProbeFailureMessage(probe));
    }

    DriverDeviceState snapshot;
    snapshot.address = runtime.logicalAddress;

    for (const auto pointName : kSemanticPointNames) {
        if (!IsSemanticPointEnabled(profile_, pointName)) {
            continue;
        }

        const auto iterator = profile_.points.find(pointName);
        if (iterator == profile_.points.end() ||
            !iterator->second.read.has_value()) {
            return MarkOffline(
                runtime,
                DriverOutcome::InvalidResponse,
                "enabled semantic point '" + std::string(pointName) +
                    "' has no readable profile location");
        }

        const RawReadResult read = ReadSemanticRegister(
            runtime.logicalAddress,
            *iterator->second.read);
        if (!read.success) {
            return MarkOffline(runtime, read.outcome, read.error);
        }

        try {
            ApplySemanticRead(
                snapshot,
                profile_,
                pointName,
                read.value);
        }
        catch (const SemanticConversionError& error) {
            return MarkOffline(
                runtime,
                DriverOutcome::InvalidResponse,
                "cannot decode semantic point '" + std::string(pointName) +
                    "': " + error.what());
        }
    }

    snapshot.online = true;
    snapshot.hasState = true;
    runtime.state = std::move(snapshot);

    return DriverResult{
        .address = runtime.logicalAddress,
        .operation = DriverOperation::PollRead,
        .outcome = DriverOutcome::Success,
        .error = {},
    };
}

ModbusDriver::RawReadResult ModbusDriver::ReadSemanticRegister(
    std::uint8_t logicalAddress,
    const RegisterLocation& baseLocation)
{
    std::optional<ResolvedRegisterLocation> location;
    try {
        location = ResolveRegisterLocation(
            profile_,
            logicalAddress,
            baseLocation);
    }
    catch (const ResolverError& error) {
        return RawReadResult{
            .outcome = DriverOutcome::InvalidResponse,
            .success = false,
            .value = 0,
            .error = error.what(),
        };
    }

    if (!location.has_value()) {
        return RawReadResult{
            .outcome = DriverOutcome::InvalidResponse,
            .success = false,
            .value = 0,
            .error =
                "profile does not resolve a configured logical address",
        };
    }

    if (location->space != RegisterSpace::HoldingRegister) {
        return RawReadResult{
            .outcome = DriverOutcome::InvalidResponse,
            .success = false,
            .value = 0,
            .error =
                "current Modbus runtime supports semantic reads from "
                "holding_register only",
        };
    }

    RtuAdu request;
    try {
        request = BuildReadHoldingRegistersRequest(
            location->slaveId,
            location->address,
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
            response.slaveId != location->slaveId ||
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
    DriverOutcome outcome,
    std::string error)
{
    runtime.state.online = false;
    return DriverResult{
        .address = runtime.logicalAddress,
        .operation = DriverOperation::PollRead,
        .outcome = outcome,
        .error = std::move(error),
    };
}

void ModbusDriver::ValidateReadablePoints(
    std::uint8_t logicalAddress) const
{
    std::size_t readableCount = 0;

    for (const auto pointName : kSemanticPointNames) {
        if (!IsSemanticPointEnabled(profile_, pointName)) {
            continue;
        }

        const auto iterator = profile_.points.find(pointName);
        if (iterator == profile_.points.end()) {
            throw std::invalid_argument(
                "profile '" + profile_.id +
                "' enables missing semantic point '" +
                std::string(pointName) + "'");
        }
        if (!iterator->second.read.has_value()) {
            throw std::invalid_argument(
                "profile '" + profile_.id +
                "' enables semantic point '" + std::string(pointName) +
                "' without a read location");
        }

        std::optional<ResolvedRegisterLocation> location;
        try {
            location = ResolveRegisterLocation(
                profile_,
                logicalAddress,
                *iterator->second.read);
        }
        catch (const ResolverError& error) {
            throw std::invalid_argument(
                "profile '" + profile_.id +
                "' cannot resolve semantic point '" +
                std::string(pointName) + "' for logical address " +
                std::to_string(logicalAddress) + ": " + error.what());
        }

        if (!location.has_value()) {
            throw std::invalid_argument(
                "profile '" + profile_.id +
                "' does not resolve semantic point '" +
                std::string(pointName) + "' for logical address " +
                std::to_string(logicalAddress));
        }

        if (location->space != RegisterSpace::HoldingRegister) {
            throw std::invalid_argument(
                "profile '" + profile_.id +
                "' uses an unsupported read data space for semantic point '" +
                std::string(pointName) + "'");
        }

        ++readableCount;
    }

    if (readableCount == 0U) {
        throw std::invalid_argument(
            "profile '" + profile_.id +
            "' exposes no readable semantic points");
    }
}

} // namespace mdv::modbus
