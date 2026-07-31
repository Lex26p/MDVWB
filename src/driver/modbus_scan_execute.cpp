#include "modbus_scan_execute.h"

#include "modbus_rtu.h"

#include <algorithm>
#include <exception>
#include <string>
#include <utility>

namespace mdv::modbus {
namespace {

[[nodiscard]] ScanResult UnsupportedCandidate(
    const ScanCandidate& candidate)
{
    return ScanResult{
        .logicalAddress = candidate.logicalAddress,
        .disposition = ScanDisposition::Unsupported,
        .reason = ScanReason::UnsupportedCandidate,
        .probe = std::nullopt,
        .exceptionCode = std::nullopt,
        .diagnostic = "profile does not support this logical candidate",
        .elapsed = {},
    };
}

[[nodiscard]] ScanResult UnsupportedDataSpace(
    const ScanProbe& probe)
{
    return ScanResult{
        .logicalAddress = probe.logicalAddress,
        .disposition = ScanDisposition::Unsupported,
        .reason = ScanReason::UnsupportedDataSpace,
        .probe = probe,
        .exceptionCode = std::nullopt,
        .diagnostic =
            "current Modbus RTU scan executor supports holding_register probes only",
        .elapsed = {},
    };
}

[[nodiscard]] ScanResult ErrorResult(
    const ScanProbe& probe,
    ScanReason reason,
    std::string diagnostic,
    std::chrono::milliseconds elapsed = {})
{
    return ScanResult{
        .logicalAddress = probe.logicalAddress,
        .disposition = ScanDisposition::Error,
        .reason = reason,
        .probe = probe,
        .exceptionCode = std::nullopt,
        .diagnostic = std::move(diagnostic),
        .elapsed = elapsed,
    };
}

[[nodiscard]] ScanResult ResultFromTransaction(
    const ScanProbe& probe,
    const TransactionResult& transaction)
{
    switch (transaction.status) {
    case TransactionStatus::Success: {
        if (!transaction.response.has_value()) {
            return ErrorResult(
                probe,
                ScanReason::InvalidResponse,
                "successful transport result did not include a parsed response",
                transaction.elapsed);
        }

        const auto& response = *transaction.response;
        if (response.status != ResponseStatus::Success ||
            response.slaveId != probe.slaveId ||
            response.function != Function::ReadHoldingRegisters ||
            response.registers.size() != probe.quantity) {
            return ErrorResult(
                probe,
                ScanReason::InvalidResponse,
                "successful probe response did not match the planned request",
                transaction.elapsed);
        }

        if (probe.presence == ProbePresence::AnyNonZero) {
            const bool anyNonZero = std::any_of(
                response.registers.begin(),
                response.registers.end(),
                [](std::uint16_t value) {
                    return value != 0U;
                });

            if (!anyNonZero) {
                return ScanResult{
                    .logicalAddress = probe.logicalAddress,
                    .disposition = ScanDisposition::NotFound,
                    .reason = ScanReason::PresenceMismatch,
                    .probe = probe,
                    .exceptionCode = std::nullopt,
                    .diagnostic =
                        "probe response did not satisfy profile presence rule",
                    .elapsed = transaction.elapsed,
                };
            }
        }

        return ScanResult{
            .logicalAddress = probe.logicalAddress,
            .disposition = ScanDisposition::Found,
            .reason = ScanReason::Success,
            .probe = probe,
            .exceptionCode = std::nullopt,
            .diagnostic = {},
            .elapsed = transaction.elapsed,
        };
    }

    case TransactionStatus::Exception: {
        ScanResult result{
            .logicalAddress = probe.logicalAddress,
            .disposition = ScanDisposition::NotFound,
            .reason = ScanReason::ExceptionResponse,
            .probe = probe,
            .exceptionCode = std::nullopt,
            .diagnostic = transaction.error,
            .elapsed = transaction.elapsed,
        };
        if (transaction.response.has_value()) {
            result.exceptionCode = transaction.response->exceptionCode;
        }
        return result;
    }

    case TransactionStatus::Timeout:
        return ScanResult{
            .logicalAddress = probe.logicalAddress,
            .disposition = ScanDisposition::NotFound,
            .reason = ScanReason::Timeout,
            .probe = probe,
            .exceptionCode = std::nullopt,
            .diagnostic = transaction.error,
            .elapsed = transaction.elapsed,
        };

    case TransactionStatus::InvalidResponse:
        return ErrorResult(
            probe,
            ScanReason::InvalidResponse,
            transaction.error,
            transaction.elapsed);

    case TransactionStatus::IoError:
        return ErrorResult(
            probe,
            ScanReason::IoError,
            transaction.error,
            transaction.elapsed);

    case TransactionStatus::InvalidRequest:
        return ErrorResult(
            probe,
            ScanReason::InvalidRequest,
            transaction.error,
            transaction.elapsed);
    }

    return ErrorResult(
        probe,
        ScanReason::InvalidResponse,
        "unknown Modbus transaction status",
        transaction.elapsed);
}

[[nodiscard]] ScanResult ExecuteProbe(
    const ScanProbe& probe,
    ITransactionTransport& transport)
{
    if (probe.space != RegisterSpace::HoldingRegister) {
        return UnsupportedDataSpace(probe);
    }

    RtuAdu request;
    try {
        request = BuildReadHoldingRegistersRequest(
            probe.slaveId,
            probe.address,
            probe.quantity);
    }
    catch (const std::exception& error) {
        return ErrorResult(
            probe,
            ScanReason::InvalidRequest,
            error.what());
    }

    return ResultFromTransaction(
        probe,
        transport.Execute(request));
}

} // namespace

ScanReport ExecuteScanPlan(
    const ScanPlan& plan,
    ITransactionTransport& transport)
{
    ScanReport report{};

    for (std::size_t index = 0; index < plan.size(); ++index) {
        const auto& candidate = plan[index];

        if (!candidate.probe.has_value()) {
            report[index] = UnsupportedCandidate(candidate);
            continue;
        }

        report[index] = ExecuteProbe(
            *candidate.probe,
            transport);
    }

    return report;
}

ScanReport ExecuteProfileScan(
    const ModbusProfile& profile,
    ITransactionTransport& transport)
{
    return ExecuteScanPlan(
        BuildScanPlan(profile),
        transport);
}

} // namespace mdv::modbus
