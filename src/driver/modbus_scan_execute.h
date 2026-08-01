#pragma once

#include "modbus_rtu_serial.h"
#include "modbus_scan.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace mdv::modbus {

enum class ScanDisposition {
    Found,
    NotFound,
    Unsupported,
    Error,
};

enum class ScanReason {
    Success,
    UnsupportedCandidate,
    UnsupportedDataSpace,
    Timeout,
    ExceptionResponse,
    PresenceMismatch,
    InvalidResponse,
    IoError,
    InvalidRequest,
};

struct ScanResult {
    std::uint8_t logicalAddress = 0;
    ScanDisposition disposition = ScanDisposition::Unsupported;
    ScanReason reason = ScanReason::UnsupportedCandidate;
    std::optional<ScanProbe> probe;
    std::optional<std::uint8_t> exceptionCode;
    std::string diagnostic;
    std::chrono::milliseconds elapsed{0};
};

using ScanReport = std::array<
    ScanResult,
    static_cast<std::size_t>(kMaxLogicalAddress)>;

// Executes one already-resolved read-only probe using the same transport,
// response validation and profile presence rule as full discovery.
[[nodiscard]] ScanResult ExecuteScanProbe(
    const ScanProbe& probe,
    ITransactionTransport& transport);

// Executes only the read-only probes already present in a scan plan.
// Unsupported candidates and currently unsupported data spaces generate no I/O.
[[nodiscard]] ScanReport ExecuteScanPlan(
    const ScanPlan& plan,
    ITransactionTransport& transport);

// Convenience wrapper that first builds the deterministic 1..63 plan.
[[nodiscard]] ScanReport ExecuteProfileScan(
    const ModbusProfile& profile,
    ITransactionTransport& transport);

} // namespace mdv::modbus
