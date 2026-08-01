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

std::uint8_t RequestFunction(const mdv::modbus::RtuAdu& request)
{
    Require(request.size() >= 2U, "Modbus request is too short");
    return request[1];
}

std::uint16_t RequestAddress(const mdv::modbus::RtuAdu& request)
{
    Require(request.size() >= 6U, "Modbus request has no address");
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(request[2]) << 8U) |
        static_cast<std::uint16_t>(request[3]));
}

std::uint16_t RequestQuantity(const mdv::modbus::RtuAdu& request)
{
    Require(request.size() >= 6U, "Modbus request has no quantity");
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(request[4]) << 8U) |
        static_cast<std::uint16_t>(request[5]));
}

std::uint16_t RequestWriteValue(const mdv::modbus::RtuAdu& request)
{
    Require(RequestFunction(request) == 0x10U, "request is not FC10");
    Require(request.size() == 11U, "one-register FC10 request size mismatch");
    Require(request[6] == 2U, "one-register FC10 byte count mismatch");
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(request[7]) << 8U) |
        static_cast<std::uint16_t>(request[8]));
}

mdv::modbus::TransactionResult ReadSuccess(
    const mdv::modbus::RtuAdu& request,
    std::vector<std::uint16_t> values)
{
    mdv::modbus::ParsedResponse response;
    response.status = mdv::modbus::ResponseStatus::Success;
    response.slaveId = request[0];
    response.function = mdv::modbus::Function::ReadHoldingRegisters;
    response.registers = std::move(values);

    mdv::modbus::TransactionResult result;
    result.status = mdv::modbus::TransactionStatus::Success;
    result.response = std::move(response);
    return result;
}

mdv::modbus::TransactionResult ReadSuccess(
    const mdv::modbus::RtuAdu& request,
    std::uint16_t value)
{
    return ReadSuccess(request, std::vector<std::uint16_t>{value});
}

mdv::modbus::TransactionResult WriteSuccess(
    const mdv::modbus::RtuAdu& request)
{
    mdv::modbus::ParsedResponse response;
    response.status = mdv::modbus::ResponseStatus::Success;
    response.slaveId = request[0];
    response.function = mdv::modbus::Function::WriteMultipleRegisters;
    response.startAddress = RequestAddress(request);
    response.quantity = RequestQuantity(request);

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
        Require(
            RequestFunction(request) == 0x03U,
            "read-only production transport received a write");
        const auto address = RequestAddress(request);

        switch (address) {
        case 40039U: return ReadSuccess(request, 24U); // logical 1 probe
        case 40028U: return ReadSuccess(request, 1U);  // logical 1 Power
        case 40035U: return ReadSuccess(request, 5U);  // logical 1 AlarmCode
        case 40130U: return ReadSuccess(request, 0U);  // logical 2 absent probe
        default:
            throw std::runtime_error(
                "unexpected production test register " +
                std::to_string(address));
        }
    }

    std::vector<mdv::modbus::RtuAdu> requests;
};

class BatchingTransport final
    : public mdv::modbus::ITransactionTransport {
public:
    [[nodiscard]] mdv::modbus::TransactionResult Execute(
        const mdv::modbus::RtuAdu& request) override
    {
        requests.push_back(request);
        Require(RequestFunction(request) == 0x03U, "batching transport received a write");
        const auto address = RequestAddress(request);
        const auto quantity = RequestQuantity(request);
        if (address == 40039U && quantity == 1U) {
            return ReadSuccess(request, 24U);
        }
        if (address == 40028U && quantity == 2U) {
            if (malformedBatch) {
                return ReadSuccess(request, std::vector<std::uint16_t>{0U});
            }
            return ReadSuccess(request, std::vector<std::uint16_t>{1U, 235U});
        }
        throw std::runtime_error(
            "unexpected batched test request " + std::to_string(address) +
            "/" + std::to_string(quantity));
    }

    bool malformedBatch = false;
    std::vector<mdv::modbus::RtuAdu> requests;
};

struct ScriptStep {
    mdv::modbus::TransactionStatus status =
        mdv::modbus::TransactionStatus::Success;
    std::uint16_t value = 0;
    std::string error;
};

class ScriptedReadTransport final
    : public mdv::modbus::ITransactionTransport {
public:
    [[nodiscard]] mdv::modbus::TransactionResult Execute(
        const mdv::modbus::RtuAdu& request) override
    {
        requests.push_back(request);
        Require(RequestFunction(request) == 0x03U, "expected FC03 request");
        if (steps.empty()) {
            throw std::runtime_error("scripted transport ran out of steps");
        }

        auto step = std::move(steps.front());
        steps.pop_front();

        if (step.status == mdv::modbus::TransactionStatus::Success) {
            return ReadSuccess(request, step.value);
        }

        mdv::modbus::TransactionResult result;
        result.status = step.status;
        result.error = std::move(step.error);
        return result;
    }

    std::deque<ScriptStep> steps;
    std::vector<mdv::modbus::RtuAdu> requests;
};

class PowerTransport final
    : public mdv::modbus::ITransactionTransport {
public:
    [[nodiscard]] mdv::modbus::TransactionResult Execute(
        const mdv::modbus::RtuAdu& request) override
    {
        requests.push_back(request);
        const auto function = RequestFunction(request);
        const auto address = RequestAddress(request);

        if (function == 0x10U) {
            Require(address == 40078U, "Power write address mismatch");
            Require(RequestQuantity(request) == 1U, "Power write quantity mismatch");
            const auto value = RequestWriteValue(request);
            if (applyWrites) {
                powerRaw = value;
            }
            return WriteSuccess(request);
        }

        Require(function == 0x03U, "unexpected Modbus function");
        switch (address) {
        case 40039U: return ReadSuccess(request, 24U);
        case 40028U: return ReadSuccess(request, powerRaw);
        case 40035U: return ReadSuccess(request, 0U);
        default:
            throw std::runtime_error(
                "unexpected Power test register " +
                std::to_string(address));
        }
    }

    bool applyWrites = true;
    std::uint16_t powerRaw = 1U;
    std::vector<mdv::modbus::RtuAdu> requests;
};

class ConfirmationTimeoutTransport final
    : public mdv::modbus::ITransactionTransport {
public:
    [[nodiscard]] mdv::modbus::TransactionResult Execute(
        const mdv::modbus::RtuAdu& request) override
    {
        requests.push_back(request);
        const auto function = RequestFunction(request);
        const auto address = RequestAddress(request);

        if (function == 0x10U && address == 40078U) {
            writeSeen = true;
            return WriteSuccess(request);
        }

        if (function == 0x03U) {
            if (address == 40039U) {
                return ReadSuccess(request, 24U);
            }
            if (address == 40028U && !writeSeen) {
                return ReadSuccess(request, 1U);
            }
            if (address == 40028U && writeSeen) {
                mdv::modbus::TransactionResult result;
                result.status = mdv::modbus::TransactionStatus::Timeout;
                result.error = "confirmation timeout";
                return result;
            }
            if (address == 40035U) {
                return ReadSuccess(request, 0U);
            }
        }

        throw std::runtime_error("unexpected confirmation-timeout request");
    }

    bool writeSeen = false;
    std::vector<mdv::modbus::RtuAdu> requests;
};

class WriteFailureTransport final
    : public mdv::modbus::ITransactionTransport {
public:
    [[nodiscard]] mdv::modbus::TransactionResult Execute(
        const mdv::modbus::RtuAdu& request) override
    {
        requests.push_back(request);
        const auto function = RequestFunction(request);
        const auto address = RequestAddress(request);

        if (function == 0x03U) {
            switch (address) {
            case 40039U: return ReadSuccess(request, 24U);
            case 40028U: return ReadSuccess(request, 1U);
            case 40035U: return ReadSuccess(request, 0U);
            default: break;
            }
        }

        if (function == 0x10U && address == 40078U) {
            mdv::modbus::TransactionResult result;
            result.status = mdv::modbus::TransactionStatus::Timeout;
            result.error = "write timeout";
            return result;
        }

        throw std::runtime_error("unexpected write-failure request");
    }

    std::vector<mdv::modbus::RtuAdu> requests;
};

mdv::modbus::ModbusProfile ProductionProfile()
{
    return mdv::modbus::LoadProfileFile(
        std::filesystem::path(MDVWB_SOURCE_DIR) /
        "profiles/modbus/vrf_add_controller.json");
}

mdv::modbus::ModbusProfile AdjacentAndSharedProfile()
{
    auto profile = ProductionProfile();
    profile.capabilities.roomTemperature = true;

    mdv::modbus::PointDefinition room;
    room.type = mdv::modbus::PointType::Number;
    room.rawType = mdv::modbus::RawType::UInt16;
    room.read = mdv::modbus::RegisterLocation{
        .space = mdv::modbus::RegisterSpace::HoldingRegister,
        .address = 40029U,
        .reference = std::nullopt,
    };
    room.transform = mdv::modbus::NumericTransform{
        .scale = 0.1,
        .offset = 0.0,
    };
    profile.points["roomTemperature"] = room;
    profile.points.at("alarmCode").read->address = 40028U;
    profile.points.at("alarmCode").read->reference.reset();
    return profile;
}

void InitializeOne(
    mdv::modbus::ModbusDriver& driver,
    bool expectedPower = true)
{
    const auto result = driver.ProcessNext();
    Require(
        result.operation == mdv::DriverOperation::PollRead &&
            result.outcome == mdv::DriverOutcome::Success,
        "initial factual snapshot failed");
    const auto state = driver.DeviceStateByAddress(1U);
    Require(state.online && state.hasState, "initial state is not factual");
    Require(state.power == expectedPower, "initial Power value mismatch");
}

void TestProductionProfileReadOnlyPolling()
{
    auto profile = ProductionProfile();
    ProductionTransport transport;
    mdv::modbus::ModbusDriver driver({1U, 2U}, profile, transport);

    Require(driver.DeviceCount() == 2U, "driver device count mismatch");
    Require(!driver.HasQueuedWork(), "new driver unexpectedly has queued work");
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

void TestAdjacentReadsAreBatchedAndSharedValuesReused()
{
    auto profile = AdjacentAndSharedProfile();
    BatchingTransport transport;
    mdv::modbus::ModbusDriver driver({1U}, profile, transport);

    const auto result = driver.ProcessNext();
    Require(result.outcome == mdv::DriverOutcome::Success, "batched poll failed");
    Require(transport.requests.size() == 2U, "batched poll used the wrong transaction count");
    Require(
        RequestAddress(transport.requests[0]) == 40039U &&
            RequestQuantity(transport.requests[0]) == 1U,
        "batched poll did not begin with the profile probe");
    Require(
        RequestAddress(transport.requests[1]) == 40028U &&
            RequestQuantity(transport.requests[1]) == 2U,
        "adjacent semantic registers were not combined");

    const auto state = driver.DeviceStateByAddress(1U);
    Require(state.online && state.hasState, "batched snapshot is not factual");
    Require(state.power, "shared Power register decoded incorrectly");
    Require(
        state.roomTemperature.has_value() &&
            *state.roomTemperature == 23.5,
        "adjacent room temperature decoded incorrectly");
    Require(state.alarmCode == 1, "shared raw register was not reused for AlarmCode");

    const auto& metrics = driver.PollPlanMetrics();
    Require(
        metrics.totalTransactionsPerCycle == 4U &&
            metrics.optimizedTotalTransactionsPerCycle == 2U &&
            metrics.savedTransactionsPerCycle == 2U,
        "driver optimization metrics do not match executed traffic");
}

void TestMalformedBatchPreservesPreviousFactualState()
{
    auto profile = AdjacentAndSharedProfile();
    BatchingTransport transport;
    mdv::modbus::ModbusDriver driver({1U}, profile, transport);

    Require(
        driver.ProcessNext().outcome == mdv::DriverOutcome::Success,
        "initial batched snapshot failed");
    const auto factual = driver.DeviceStateByAddress(1U);

    transport.malformedBatch = true;
    const auto failed = driver.ProcessNext();
    Require(
        failed.outcome == mdv::DriverOutcome::InvalidResponse,
        "malformed batch was not rejected");

    const auto after = driver.DeviceStateByAddress(1U);
    Require(!after.online, "malformed batch left the device online");
    Require(after.hasState, "malformed batch erased the prior factual snapshot");
    Require(after.power == factual.power, "malformed batch changed factual Power");
    Require(
        after.roomTemperature == factual.roomTemperature,
        "malformed batch changed factual room temperature");
    Require(
        after.alarmCode == factual.alarmCode,
        "malformed batch changed factual AlarmCode");
}

void TestFailedSnapshotDoesNotPublishPartialValues()
{
    auto profile = ProductionProfile();
    ScriptedReadTransport transport;

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

void TestPowerWriteRequiresFactualState()
{
    auto profile = ProductionProfile();
    PowerTransport transport;
    mdv::modbus::ModbusDriver driver({1U}, profile, transport);

    bool rejected = false;
    try {
        driver.ApplyCommand(mdv::DriverCommand{
            .address = 1U,
            .control = mdv::DriverControl::Power,
            .value = false,
        });
    }
    catch (const std::logic_error&) {
        rejected = true;
    }

    Require(rejected, "Power command before factual read was accepted");
    Require(transport.requests.empty(), "rejected command generated traffic");
}

void TestPowerWriteAndReadBackConfirmation()
{
    auto profile = ProductionProfile();
    PowerTransport transport;
    mdv::modbus::ModbusDriver driver({1U}, profile, transport);
    InitializeOne(driver);
    transport.requests.clear();

    driver.ApplyCommand(mdv::DriverCommand{
        .address = 1U,
        .control = mdv::DriverControl::Power,
        .value = false,
    });
    Require(driver.HasQueuedWork(), "Power command was not queued");

    const auto write = driver.ProcessNext();
    Require(
        write.operation == mdv::DriverOperation::SetState &&
            write.outcome == mdv::DriverOutcome::Success,
        "FC10 Power write failed");
    Require(transport.requests.size() == 1U, "Power write request count mismatch");
    Require(RequestFunction(transport.requests[0]) == 0x10U, "Power write did not use FC10");
    Require(RequestAddress(transport.requests[0]) == 40078U, "Power write register mismatch");
    Require(RequestQuantity(transport.requests[0]) == 1U, "Power write quantity mismatch");
    Require(RequestWriteValue(transport.requests[0]) == 0U, "Power OFF raw value mismatch");

    const auto afterWrite = driver.DeviceStateByAddress(1U);
    Require(afterWrite.power, "FC10 acknowledgement changed factual Power prematurely");
    Require(driver.HasQueuedWork(), "read-back confirmation was not queued");

    const auto confirmation = driver.ProcessNext();
    Require(
        confirmation.operation == mdv::DriverOperation::ConfirmRead &&
            confirmation.outcome == mdv::DriverOutcome::Success,
        "Power read-back confirmation failed");
    Require(transport.requests.size() == 2U, "confirmation request count mismatch");
    Require(RequestFunction(transport.requests[1]) == 0x03U, "confirmation did not use FC03");
    Require(RequestAddress(transport.requests[1]) == 40028U, "confirmation read wrong register");

    const auto confirmed = driver.DeviceStateByAddress(1U);
    Require(confirmed.online, "confirmed device is not online");
    Require(confirmed.hasState, "confirmed device lost factual state");
    Require(!confirmed.power, "confirmed Power state did not change");
    Require(!driver.HasQueuedWork(), "confirmed command remained queued");
}

void TestWriteTimeoutRetriesAreBounded()
{
    auto profile = ProductionProfile();
    WriteFailureTransport transport;
    mdv::modbus::ModbusDriver driver({1U}, profile, transport);
    InitializeOne(driver);
    transport.requests.clear();

    driver.ApplyCommand(mdv::DriverCommand{
        .address = 1U,
        .control = mdv::DriverControl::Power,
        .value = false,
    });

    for (std::uint32_t attempt = 1;
         attempt <= mdv::modbus::kMaxModbusWriteAttempts;
         ++attempt) {
        const auto result = driver.ProcessNext();
        Require(
            result.operation == mdv::DriverOperation::SetState,
            "write retry operation mismatch");
        Require(
            result.outcome == mdv::DriverOutcome::Timeout,
            "write timeout outcome mismatch");
        Require(
            driver.HasQueuedWork() ==
                (attempt < mdv::modbus::kMaxModbusWriteAttempts),
            "write retry budget mismatch");
    }

    Require(
        transport.requests.size() == mdv::modbus::kMaxModbusWriteAttempts,
        "unexpected FC10 retry count");
    for (const auto& request : transport.requests) {
        Require(RequestFunction(request) == 0x10U, "retry was not FC10");
    }
    Require(
        driver.DeviceStateByAddress(1U).power,
        "failed writes changed factual Power");
}

void TestConfirmationTimeoutRetriesAreBounded()
{
    auto profile = ProductionProfile();
    ConfirmationTimeoutTransport transport;
    mdv::modbus::ModbusDriver driver({1U}, profile, transport);
    InitializeOne(driver);
    transport.requests.clear();

    driver.ApplyCommand(mdv::DriverCommand{
        .address = 1U,
        .control = mdv::DriverControl::Power,
        .value = false,
    });

    const auto write = driver.ProcessNext();
    Require(write.outcome == mdv::DriverOutcome::Success, "FC10 before confirmation failed");

    for (std::uint32_t attempt = 1;
         attempt <= mdv::modbus::kMaxModbusConfirmationAttempts;
         ++attempt) {
        const auto result = driver.ProcessNext();
        Require(
            result.operation == mdv::DriverOperation::ConfirmRead,
            "confirmation retry operation mismatch");
        Require(
            result.outcome == mdv::DriverOutcome::Timeout,
            "confirmation timeout outcome mismatch");
        Require(
            driver.HasQueuedWork() ==
                (attempt < mdv::modbus::kMaxModbusConfirmationAttempts),
            "confirmation retry budget mismatch");
    }

    Require(
        !driver.DeviceStateByAddress(1U).online,
        "exhausted confirmation failures left device online");
    Require(
        driver.DeviceStateByAddress(1U).power,
        "failed confirmations changed factual Power");
}

void TestConfirmationMismatchRetriesWrite()
{
    auto profile = ProductionProfile();
    PowerTransport transport;
    transport.applyWrites = false;
    mdv::modbus::ModbusDriver driver({1U}, profile, transport);
    InitializeOne(driver);
    transport.requests.clear();

    driver.ApplyCommand(mdv::DriverCommand{
        .address = 1U,
        .control = mdv::DriverControl::Power,
        .value = false,
    });

    const auto write = driver.ProcessNext();
    Require(write.outcome == mdv::DriverOutcome::Success, "initial FC10 failed");

    const auto mismatch = driver.ProcessNext();
    Require(
        mismatch.operation == mdv::DriverOperation::SetState &&
            mismatch.outcome == mdv::DriverOutcome::InvalidResponse,
        "Power mismatch classification is wrong");
    Require(driver.HasQueuedWork(), "mismatch did not queue another write");
    Require(driver.DeviceStateByAddress(1U).online, "valid mismatch marked device offline");
    Require(driver.DeviceStateByAddress(1U).power, "mismatch changed factual Power");

    const auto retry = driver.ProcessNext();
    Require(
        retry.operation == mdv::DriverOperation::SetState &&
            retry.outcome == mdv::DriverOutcome::Success,
        "mismatch write retry failed");
    Require(RequestFunction(transport.requests.back()) == 0x10U, "mismatch did not retry FC10");
}

void TestPriorityWorkCannotStarvePolling()
{
    auto profile = ProductionProfile();
    PowerTransport transport;
    transport.applyWrites = false;
    mdv::modbus::ModbusDriver driver({1U}, profile, transport);
    InitializeOne(driver);
    transport.requests.clear();

    driver.ApplyCommand(mdv::DriverCommand{
        .address = 1U,
        .control = mdv::DriverControl::Power,
        .value = false,
    });

    for (std::size_t index = 0;
         index < mdv::modbus::kMaxModbusPriorityOperationsBeforePoll;
         ++index) {
        static_cast<void>(driver.ProcessNext());
    }

    const auto requestsBeforePoll = transport.requests.size();
    const auto poll = driver.ProcessNext();
    Require(
        poll.operation == mdv::DriverOperation::PollRead,
        "bounded command burst did not yield to ordinary polling");
    Require(
        transport.requests.size() == requestsBeforePoll + 3U,
        "ordinary factual poll did not execute a full snapshot");
    Require(
        RequestFunction(transport.requests[requestsBeforePoll]) == 0x03U &&
            RequestAddress(transport.requests[requestsBeforePoll]) == 40039U,
        "fairness poll did not begin with the presence probe");
}

void TestNewerCommandCancelsStaleWork()
{
    auto profile = ProductionProfile();
    PowerTransport transport;
    mdv::modbus::ModbusDriver driver({1U}, profile, transport);
    InitializeOne(driver);
    transport.requests.clear();

    driver.ApplyCommand(mdv::DriverCommand{
        .address = 1U,
        .control = mdv::DriverControl::Power,
        .value = false,
    });
    Require(driver.HasQueuedWork(), "first command was not queued");

    driver.ApplyCommand(mdv::DriverCommand{
        .address = 1U,
        .control = mdv::DriverControl::Power,
        .value = true,
    });
    Require(!driver.HasQueuedWork(), "new factual-value command did not cancel stale work");

    const auto next = driver.ProcessNext();
    Require(next.operation == mdv::DriverOperation::PollRead, "stale write escaped cancellation");
    Require(
        std::none_of(
            transport.requests.begin(),
            transport.requests.end(),
            [](const mdv::modbus::RtuAdu& request) {
                return RequestFunction(request) == 0x10U;
            }),
        "cancelled command generated FC10 traffic");
}

void TestUnsupportedCommandsGenerateNoTraffic()
{
    auto profile = ProductionProfile();
    PowerTransport transport;
    mdv::modbus::ModbusDriver driver({1U}, profile, transport);
    InitializeOne(driver);
    transport.requests.clear();

    bool modeRejected = false;
    try {
        driver.ApplyCommand(mdv::DriverCommand{
            .address = 1U,
            .control = mdv::DriverControl::Mode,
            .value = mdv::HvacMode::Cool,
        });
    }
    catch (const std::invalid_argument&) {
        modeRejected = true;
    }

    bool typeRejected = false;
    try {
        driver.ApplyCommand(mdv::DriverCommand{
            .address = 1U,
            .control = mdv::DriverControl::Power,
            .value = 22.0,
        });
    }
    catch (const std::invalid_argument&) {
        typeRejected = true;
    }

    Require(modeRejected, "disabled Mode command was accepted");
    Require(typeRejected, "non-boolean Power command was accepted");
    Require(transport.requests.empty(), "rejected commands generated traffic");
}


void TestResolvedPollPlanBaselineIsExposed()
{
    auto profile = ProductionProfile();
    ScriptedReadTransport transport;
    mdv::modbus::ModbusDriver driver({1U, 2U}, profile, transport);

    const auto& metrics = driver.PollPlanMetrics();
    Require(metrics.deviceCount == 2U, "driver poll plan lost devices");
    Require(
        metrics.probeTransactionsPerCycle == 2U,
        "driver probe baseline is wrong");
    Require(
        metrics.semanticTransactionsPerCycle == 4U,
        "driver semantic baseline is wrong");
    Require(
        metrics.totalTransactionsPerCycle == 6U,
        "driver total transaction baseline is wrong");
    Require(
        metrics.registersRequestedPerCycle == 6U,
        "driver register baseline is wrong");
    Require(
        metrics.optimizedSemanticTransactionsPerCycle == 4U &&
            metrics.optimizedTotalTransactionsPerCycle == 6U &&
            metrics.optimizedRegistersRequestedPerCycle == 6U &&
            metrics.reusedSemanticReadsPerCycle == 0U &&
            metrics.savedTransactionsPerCycle == 0U,
        "driver production optimization metrics are wrong");
    Require(
        transport.requests.empty(),
        "building the resolved poll plan generated Modbus traffic");
}

void TestInvalidConfiguredAddressesRejected()
{
    auto profile = ProductionProfile();
    ScriptedReadTransport transport;

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
        TestAdjacentReadsAreBatchedAndSharedValuesReused();
        TestMalformedBatchPreservesPreviousFactualState();
        TestFailedSnapshotDoesNotPublishPartialValues();
        TestPowerWriteRequiresFactualState();
        TestPowerWriteAndReadBackConfirmation();
        TestWriteTimeoutRetriesAreBounded();
        TestConfirmationTimeoutRetriesAreBounded();
        TestConfirmationMismatchRetriesWrite();
        TestPriorityWorkCannotStarvePolling();
        TestNewerCommandCancelsStaleWork();
        TestUnsupportedCommandsGenerateNoTraffic();
        TestResolvedPollPlanBaselineIsExposed();
        TestInvalidConfiguredAddressesRejected();

        std::cout << "MDVWB Modbus driver command tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB Modbus driver command tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
