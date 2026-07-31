#include "modbus_scan_execute.h"

#include "modbus_rtu.h"

#include <cstdint>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

mdv::modbus::ModbusProfile DirectProfile(
    std::uint8_t logicalMax = 63U)
{
    mdv::modbus::ModbusProfile profile;
    profile.id = "scan_execute_test";
    profile.name = "Scan Execute Test";
    profile.registerAddressing = "pdu_zero_based";
    profile.addressing = mdv::modbus::DirectSlaveAddressing{
        .logicalMin = 1U,
        .logicalMax = logicalMax,
        .registerOffset = 0U,
    };
    profile.probe.read = mdv::modbus::RegisterLocation{
        .space = mdv::modbus::RegisterSpace::HoldingRegister,
        .address = 100U,
    };
    profile.probe.quantity = 2U;
    return profile;
}

mdv::modbus::TransactionResult SuccessForRequest(
    const mdv::modbus::RtuAdu& request)
{
    if (request.size() != 8U) {
        throw std::runtime_error("unexpected request size in fake transport");
    }

    const auto quantity = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(request[4]) << 8U) |
        static_cast<std::uint16_t>(request[5]));

    mdv::modbus::ParsedResponse response;
    response.status = mdv::modbus::ResponseStatus::Success;
    response.slaveId = request[0];
    response.function = mdv::modbus::Function::ReadHoldingRegisters;
    response.registers.assign(quantity, 0U);

    mdv::modbus::TransactionResult result;
    result.status = mdv::modbus::TransactionStatus::Success;
    result.response = std::move(response);
    return result;
}

class AlwaysSuccessTransport final
    : public mdv::modbus::ITransactionTransport {
public:
    [[nodiscard]] mdv::modbus::TransactionResult Execute(
        const mdv::modbus::RtuAdu& request) override
    {
        requests.push_back(request);
        return SuccessForRequest(request);
    }

    std::vector<mdv::modbus::RtuAdu> requests;
};

class ScriptedTransport final
    : public mdv::modbus::ITransactionTransport {
public:
    [[nodiscard]] mdv::modbus::TransactionResult Execute(
        const mdv::modbus::RtuAdu& request) override
    {
        requests.push_back(request);
        if (results.empty()) {
            throw std::runtime_error("scripted transport ran out of results");
        }

        auto result = std::move(results.front());
        results.pop_front();

        if (result.status == mdv::modbus::TransactionStatus::Success &&
            !result.response.has_value()) {
            return SuccessForRequest(request);
        }

        return result;
    }

    std::deque<mdv::modbus::TransactionResult> results;
    std::vector<mdv::modbus::RtuAdu> requests;
};

mdv::modbus::TransactionResult Status(
    mdv::modbus::TransactionStatus status,
    std::string error)
{
    mdv::modbus::TransactionResult result;
    result.status = status;
    result.error = std::move(error);
    return result;
}

void TestFullDirectScanExecutesAll63ReadOnlyProbes()
{
    auto profile = DirectProfile();
    AlwaysSuccessTransport transport;

    const auto report =
        mdv::modbus::ExecuteProfileScan(profile, transport);

    Require(report.size() == 63U, "scan report does not contain 63 results");
    Require(
        transport.requests.size() == 63U,
        "full direct scan did not execute exactly 63 probes");

    for (std::size_t index = 0; index < report.size(); ++index) {
        const auto logical = static_cast<std::uint8_t>(index + 1U);
        const auto& result = report[index];

        Require(
            result.logicalAddress == logical,
            "scan report order is not 1..63");
        Require(
            result.disposition == mdv::modbus::ScanDisposition::Found,
            "successful direct probe was not reported as Found");

        const auto expected =
            mdv::modbus::BuildReadHoldingRegistersRequest(
                logical,
                100U,
                2U);
        Require(
            transport.requests[index] == expected,
            "scan executor built an unexpected Modbus request");
    }
}

void TestTransportOutcomesAreClassifiedDeterministically()
{
    auto profile = DirectProfile(7U);
    ScriptedTransport transport;

    transport.results.push_back(
        mdv::modbus::TransactionResult{
            .status = mdv::modbus::TransactionStatus::Success,
        });

    transport.results.push_back(
        Status(
            mdv::modbus::TransactionStatus::Timeout,
            "timeout"));

    mdv::modbus::ParsedResponse exception;
    exception.status = mdv::modbus::ResponseStatus::Exception;
    exception.slaveId = 3U;
    exception.function =
        mdv::modbus::Function::ReadHoldingRegisters;
    exception.exceptionCode = 2U;
    transport.results.push_back(
        mdv::modbus::TransactionResult{
            .status = mdv::modbus::TransactionStatus::Exception,
            .response = exception,
            .error = "Modbus exception code 2",
        });

    transport.results.push_back(
        Status(
            mdv::modbus::TransactionStatus::InvalidResponse,
            "bad response"));

    transport.results.push_back(
        Status(
            mdv::modbus::TransactionStatus::IoError,
            "serial failure"));

    transport.results.push_back(
        Status(
            mdv::modbus::TransactionStatus::InvalidRequest,
            "bad request"));

    mdv::modbus::ParsedResponse wrongCount;
    wrongCount.status = mdv::modbus::ResponseStatus::Success;
    wrongCount.slaveId = 7U;
    wrongCount.function =
        mdv::modbus::Function::ReadHoldingRegisters;
    wrongCount.registers = {1U};
    transport.results.push_back(
        mdv::modbus::TransactionResult{
            .status = mdv::modbus::TransactionStatus::Success,
            .response = wrongCount,
        });

    const auto report =
        mdv::modbus::ExecuteProfileScan(profile, transport);

    Require(
        report[0].disposition == mdv::modbus::ScanDisposition::Found &&
            report[0].reason == mdv::modbus::ScanReason::Success,
        "success was not classified as Found");

    Require(
        report[1].disposition == mdv::modbus::ScanDisposition::NotFound &&
            report[1].reason == mdv::modbus::ScanReason::Timeout,
        "timeout was not classified as NotFound");

    Require(
        report[2].disposition == mdv::modbus::ScanDisposition::NotFound &&
            report[2].reason == mdv::modbus::ScanReason::ExceptionResponse &&
            report[2].exceptionCode == 2U,
        "exception response classification mismatch");

    Require(
        report[3].disposition == mdv::modbus::ScanDisposition::Error &&
            report[3].reason == mdv::modbus::ScanReason::InvalidResponse,
        "invalid response was not classified as Error");

    Require(
        report[4].disposition == mdv::modbus::ScanDisposition::Error &&
            report[4].reason == mdv::modbus::ScanReason::IoError,
        "I/O failure was not classified as Error");

    Require(
        report[5].disposition == mdv::modbus::ScanDisposition::Error &&
            report[5].reason == mdv::modbus::ScanReason::InvalidRequest,
        "invalid request was not classified as Error");

    Require(
        report[6].disposition == mdv::modbus::ScanDisposition::Error &&
            report[6].reason == mdv::modbus::ScanReason::InvalidResponse,
        "malformed success was not rejected");

    Require(
        transport.requests.size() == 7U,
        "supported candidates did not execute exactly once");

    for (std::size_t index = 7U; index < report.size(); ++index) {
        Require(
            report[index].disposition ==
                mdv::modbus::ScanDisposition::Unsupported &&
            report[index].reason ==
                mdv::modbus::ScanReason::UnsupportedCandidate,
            "out-of-profile logical candidate was not reported Unsupported");
    }
}

void TestUnsupportedCandidatesCauseNoBusTraffic()
{
    auto profile = DirectProfile(1U);
    AlwaysSuccessTransport transport;

    const auto report =
        mdv::modbus::ExecuteProfileScan(profile, transport);

    Require(
        transport.requests.size() == 1U,
        "unsupported logical candidates generated Modbus traffic");

    for (std::size_t index = 1U; index < report.size(); ++index) {
        Require(
            report[index].disposition ==
                mdv::modbus::ScanDisposition::Unsupported,
            "unsupported candidate disposition mismatch");
    }
}

void TestUnsupportedProbeSpaceCausesNoBusTraffic()
{
    auto profile = DirectProfile(2U);
    profile.probe.read.space =
        mdv::modbus::RegisterSpace::InputRegister;

    AlwaysSuccessTransport transport;

    const auto report =
        mdv::modbus::ExecuteProfileScan(profile, transport);

    Require(
        transport.requests.empty(),
        "unsupported probe data space generated bus traffic");

    Require(
        report[0].disposition ==
            mdv::modbus::ScanDisposition::Unsupported &&
        report[0].reason ==
            mdv::modbus::ScanReason::UnsupportedDataSpace,
        "input-register probe was not reported as unsupported");

    Require(
        report[1].disposition ==
            mdv::modbus::ScanDisposition::Unsupported &&
        report[1].reason ==
            mdv::modbus::ScanReason::UnsupportedDataSpace,
        "second input-register probe was not reported as unsupported");
}

void TestExceptionIsNotReportedAsFound()
{
    auto profile = DirectProfile(1U);
    ScriptedTransport transport;

    mdv::modbus::ParsedResponse exception;
    exception.status = mdv::modbus::ResponseStatus::Exception;
    exception.slaveId = 1U;
    exception.function =
        mdv::modbus::Function::ReadHoldingRegisters;
    exception.exceptionCode = 3U;

    transport.results.push_back(
        mdv::modbus::TransactionResult{
            .status = mdv::modbus::TransactionStatus::Exception,
            .response = exception,
            .error = "Modbus exception code 3",
        });

    const auto report =
        mdv::modbus::ExecuteProfileScan(profile, transport);

    Require(
        report[0].disposition != mdv::modbus::ScanDisposition::Found,
        "exception response incorrectly discovered a device");
}

} // namespace

int main()
{
    try {
        TestFullDirectScanExecutesAll63ReadOnlyProbes();
        TestTransportOutcomesAreClassifiedDeterministically();
        TestUnsupportedCandidatesCauseNoBusTraffic();
        TestUnsupportedProbeSpaceCausesNoBusTraffic();
        TestExceptionIsNotReportedAsFound();

        std::cout << "MDVWB Modbus scan execution tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB Modbus scan execution tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
