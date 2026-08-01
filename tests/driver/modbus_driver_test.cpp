#include "modbus_driver.h"

#include "modbus_profile.h"
#include "modbus_rtu.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef MDVWB_SOURCE_DIR
#error MDVWB_SOURCE_DIR must point to the repository source directory
#endif

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::uint16_t RequestAddress(const mdv::modbus::RtuAdu& request)
{
    Require(request.size() == 8U, "unexpected FC03 request size");
    Require(request[1] == 0x03U, "unexpected Modbus function");
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(request[2]) << 8U) |
        static_cast<std::uint16_t>(request[3]));
}

mdv::modbus::TransactionResult Success(
    const mdv::modbus::RtuAdu& request,
    std::uint16_t value)
{
    mdv::modbus::ParsedResponse response;
    response.status = mdv::modbus::ResponseStatus::Success;
    response.slaveId = request[0];
    response.function =
        mdv::modbus::Function::ReadHoldingRegisters;
    response.registers = {value};

    mdv::modbus::TransactionResult result;
    result.status = mdv::modbus::TransactionStatus::Success;
    result.response = std::move(response);
    return result;
}

class ProductionTransport final
    : public mdv::modbus::ITransactionTransport {
public:
    [[nodiscard]] mdv::modbus::TransactionResult Execute(
        const mdv::modbus::RtuAdu& request) override
    {
        requests.push_back(request);
        const auto address = RequestAddress(request);

        switch (address) {
        case 40039U: return Success(request, 24U); // logical 1 probe
        case 40028U: return Success(request, 1U);  // logical 1 Power
        case 40035U: return Success(request, 5U);  // logical 1 AlarmCode
        case 40130U: return Success(request, 0U);  // logical 2 absent probe
        default:
            throw std::runtime_error(
                "unexpected production test register " +
                std::to_string(address));
        }
    }

    std::vector<mdv::modbus::RtuAdu> requests;
};

struct ScriptStep {
    mdv::modbus::TransactionStatus status =
        mdv::modbus::TransactionStatus::Success;
    std::uint16_t value = 0;
    std::string error;
};

class ScriptedTransport final
    : public mdv::modbus::ITransactionTransport {
public:
    [[nodiscard]] mdv::modbus::TransactionResult Execute(
        const mdv::modbus::RtuAdu& request) override
    {
        requests.push_back(request);
        if (steps.empty()) {
            throw std::runtime_error("scripted transport ran out of steps");
        }

        auto step = std::move(steps.front());
        steps.pop_front();

        if (step.status == mdv::modbus::TransactionStatus::Success) {
            return Success(request, step.value);
        }

        mdv::modbus::TransactionResult result;
        result.status = step.status;
        result.error = std::move(step.error);
        return result;
    }

    std::deque<ScriptStep> steps;
    std::vector<mdv::modbus::RtuAdu> requests;
};

mdv::modbus::ModbusProfile ProductionProfile()
{
    return mdv::modbus::LoadProfileFile(
        std::filesystem::path(MDVWB_SOURCE_DIR) /
        "profiles/modbus/vrf_add_controller.json");
}

void TestProductionProfileReadOnlyPolling()
{
    auto profile = ProductionProfile();
    ProductionTransport transport;
    mdv::modbus::ModbusDriver driver({1U, 2U}, profile, transport);

    Require(driver.DeviceCount() == 2U, "driver device count mismatch");
    Require(!driver.HasQueuedWork(), "read-only driver unexpectedly has queued work");
    Require(driver.NextPollAddress() == 1U, "initial poll address mismatch");

    const auto first = driver.ProcessNext();
    Require(first.address == 1U, "first poll logical address mismatch");
    Require(
        first.operation == mdv::DriverOperation::PollRead,
        "first Modbus operation is not PollRead");
    Require(
        first.outcome == mdv::DriverOutcome::Success,
        "present logical device did not produce Success");

    const auto state = driver.DeviceStateByAddress(1U);
    Require(state.address == 1U, "state logical address mismatch");
    Require(state.online, "successful snapshot did not mark device online");
    Require(state.hasState, "successful snapshot did not publish factual state");
    Require(state.power, "Power semantic value mismatch");
    Require(state.alarmCode == 5, "AlarmCode semantic value mismatch");

    Require(
        transport.requests.size() == 3U,
        "one factual snapshot must use probe + two enabled semantic reads");
    Require(
        RequestAddress(transport.requests[0]) == 40039U,
        "snapshot did not begin with the profile presence probe");
    Require(
        RequestAddress(transport.requests[1]) == 40028U,
        "Power read address mismatch");
    Require(
        RequestAddress(transport.requests[2]) == 40035U,
        "AlarmCode read address mismatch");

    Require(driver.NextPollAddress() == 2U, "round robin did not advance");

    const auto second = driver.ProcessNext();
    Require(second.address == 2U, "second poll logical address mismatch");
    Require(
        second.outcome == mdv::DriverOutcome::Timeout,
        "zero presence probe must use ordinary offline outcome");

    const auto missing = driver.DeviceStateByAddress(2U);
    Require(!missing.online, "absent logical device was marked online");
    Require(!missing.hasState, "absent logical device acquired factual state");

    Require(
        transport.requests.size() == 4U,
        "absent logical device must stop after the presence probe");
    Require(
        RequestAddress(transport.requests[3]) == 40130U,
        "logical 2 probe did not apply +91 register stride");
    Require(driver.NextPollAddress() == 1U, "round robin did not wrap");
}

void TestFailedSnapshotDoesNotPublishPartialValues()
{
    auto profile = ProductionProfile();
    ScriptedTransport transport;

    transport.steps = {
        {mdv::modbus::TransactionStatus::Success, 24U, {}},
        {mdv::modbus::TransactionStatus::Success, 1U, {}},
        {mdv::modbus::TransactionStatus::Success, 5U, {}},
        {mdv::modbus::TransactionStatus::Success, 25U, {}},
        {mdv::modbus::TransactionStatus::Success, 0U, {}},
        {mdv::modbus::TransactionStatus::InvalidResponse, 0U, "bad alarm response"},
    };

    mdv::modbus::ModbusDriver driver({1U}, profile, transport);

    const auto initial = driver.ProcessNext();
    Require(
        initial.outcome == mdv::DriverOutcome::Success,
        "initial factual snapshot failed");

    const auto before = driver.DeviceStateByAddress(1U);
    Require(before.online && before.power && before.alarmCode == 5,
            "initial factual snapshot values mismatch");

    const auto failed = driver.ProcessNext();
    Require(
        failed.outcome == mdv::DriverOutcome::InvalidResponse,
        "invalid semantic response classification mismatch");

    const auto after = driver.DeviceStateByAddress(1U);
    Require(!after.online, "failed snapshot did not mark device offline");
    Require(after.hasState, "failed snapshot destroyed last confirmed state");
    Require(after.power, "partial Power read overwrote confirmed snapshot");
    Require(
        after.alarmCode == 5,
        "failed snapshot overwrote confirmed AlarmCode");
}

void TestIoFailureClassification()
{
    auto profile = ProductionProfile();
    ScriptedTransport transport;
    transport.steps = {
        {mdv::modbus::TransactionStatus::IoError, 0U, "serial disconnected"},
    };

    mdv::modbus::ModbusDriver driver({1U}, profile, transport);
    const auto result = driver.ProcessNext();

    Require(
        result.outcome == mdv::DriverOutcome::IoError,
        "probe I/O failure was not propagated");
    Require(
        !driver.DeviceStateByAddress(1U).online,
        "I/O failure left device online");
}

void TestUnsupportedReadSpaceRejectedBeforeTraffic()
{
    auto profile = ProductionProfile();
    profile.points.at("power").read->space =
        mdv::modbus::RegisterSpace::InputRegister;

    ScriptedTransport transport;
    bool rejected = false;
    try {
        mdv::modbus::ModbusDriver driver({1U}, profile, transport);
        static_cast<void>(driver);
    }
    catch (const std::invalid_argument&) {
        rejected = true;
    }

    Require(rejected, "unsupported semantic read space was accepted");
    Require(
        transport.requests.empty(),
        "constructor validation generated bus traffic");
}

void TestWritesRemainDisabledAndGenerateNoTraffic()
{
    auto profile = ProductionProfile();
    ScriptedTransport transport;
    mdv::modbus::ModbusDriver driver({1U}, profile, transport);

    bool rejected = false;
    try {
        driver.ApplyCommand(mdv::DriverCommand{
            .address = 1U,
            .control = mdv::DriverControl::Power,
            .value = true,
        });
    }
    catch (const std::logic_error&) {
        rejected = true;
    }

    Require(rejected, "read-only Modbus driver accepted a write command");
    Require(
        transport.requests.empty(),
        "rejected write command generated Modbus traffic");
}

void TestInvalidConfiguredAddressesRejected()
{
    auto profile = ProductionProfile();
    ScriptedTransport transport;

    bool zeroRejected = false;
    try {
        mdv::modbus::ModbusDriver driver({0U}, profile, transport);
        static_cast<void>(driver);
    }
    catch (const std::invalid_argument&) {
        zeroRejected = true;
    }

    bool duplicateRejected = false;
    try {
        mdv::modbus::ModbusDriver driver({1U, 1U}, profile, transport);
        static_cast<void>(driver);
    }
    catch (const std::invalid_argument&) {
        duplicateRejected = true;
    }

    Require(zeroRejected, "logical address 0 was accepted");
    Require(duplicateRejected, "duplicate logical address was accepted");
    Require(
        transport.requests.empty(),
        "configuration validation generated bus traffic");
}

} // namespace

int main()
{
    try {
        TestProductionProfileReadOnlyPolling();
        TestFailedSnapshotDoesNotPublishPartialValues();
        TestIoFailureClassification();
        TestUnsupportedReadSpaceRejectedBeforeTraffic();
        TestWritesRemainDisabledAndGenerateNoTraffic();
        TestInvalidConfiguredAddressesRejected();

        std::cout << "MDVWB Modbus driver read-only tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB Modbus driver read-only tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
