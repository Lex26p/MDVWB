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
        const auto quantity = RequestQuantity(request);

        switch (address) {
        case 40039U: return ReadSuccess(request, 24U); // logical 1 probe
        case 40028U:
            Require(quantity == 4U, "production state batch size mismatch");
            return ReadSuccess(
                request,
                std::vector<std::uint16_t>{1U, 2U, 1U, 24U});
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
            if (RequestQuantity(request) == 4U) {
                return ReadSuccess(
                    request,
                    std::vector<std::uint16_t>{
                        step.value, 1U, 1U, 24U});
            }
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
            Require(RequestQuantity(request) == 1U, "write quantity mismatch");
            const auto value = RequestWriteValue(request);
            if (applyWrites) {
                switch (address) {
                case 40078U: powerRaw = value; break;
                case 40079U: modeRaw = value; break;
                case 40080U: fanSpeedRaw = value; break;
                case 40081U: setTemperatureRaw = value; break;
                default:
                    throw std::runtime_error(
                        "unexpected write register " +
                        std::to_string(address));
                }
            }
            return WriteSuccess(request);
        }

        Require(function == 0x03U, "unexpected Modbus function");
        switch (address) {
        case 40039U: return ReadSuccess(request, 24U);
        case 40028U:
            if (RequestQuantity(request) == 4U) {
                return ReadSuccess(
                    request,
                    std::vector<std::uint16_t>{
                        powerRaw,
                        modeRaw,
                        fanSpeedRaw,
                        setTemperatureRaw});
            }
            return ReadSuccess(request, powerRaw);
        case 40029U: return ReadSuccess(request, modeRaw);
        case 40030U: return ReadSuccess(request, fanSpeedRaw);
        case 40031U: return ReadSuccess(request, setTemperatureRaw);
        case 40035U: return ReadSuccess(request, 0U);
        default:
            throw std::runtime_error(
                "unexpected Power test register " +
                std::to_string(address));
        }
    }

    bool applyWrites = true;
    std::uint16_t powerRaw = 1U;
    std::uint16_t modeRaw = 1U;
    std::uint16_t fanSpeedRaw = 1U;
    std::uint16_t setTemperatureRaw = 24U;
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
                return ReadSuccess(
                    request,
                    std::vector<std::uint16_t>{1U, 1U, 1U, 24U});
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

class InvalidConfirmationTransport final
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
            if (address == 40028U) {
                if (!writeSeen) {
                    return ReadSuccess(
                        request,
                        std::vector<std::uint16_t>{1U, 1U, 1U, 24U});
                }
                return ReadSuccess(request, 2U);
            }
            if (address == 40035U) {
                return ReadSuccess(request, 0U);
            }
        }

        throw std::runtime_error(
            "unexpected invalid-confirmation request");
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
            case 40028U:
                return ReadSuccess(
                    request,
                    std::vector<std::uint16_t>{1U, 1U, 1U, 24U});
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
    profile.capabilities.mode = false;
    profile.capabilities.fanSpeed = false;
    profile.capabilities.setTemperature = false;
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
    Require(
        state.mode == mdv::HvacMode::Cool,
        "Mode semantic value mismatch");
    Require(
        state.fanSpeed == mdv::HvacFanSpeed::Auto,
        "FanSpeed semantic value mismatch");
    Require(
        state.setTemperature == 24.0,
        "SetTemperature semantic value mismatch");
    Require(
        state.roomTemperature == 24.0,
        "integer RoomTemperature semantic value mismatch");
    Require(state.alarmCode == 5, "AlarmCode semantic value mismatch");

    Require(
        transport.requests.size() == 3U,
        "one factual snapshot must use probe + two enabled semantic reads");
    Require(
        RequestAddress(transport.requests[0]) == 40039U,
        "snapshot did not begin with the profile presence probe");
    Require(
        RequestAddress(transport.requests[1]) == 40028U &&
            RequestQuantity(transport.requests[1]) == 4U,
        "factual state batch mismatch");
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
    for (std::uint32_t failure = 1;
         failure < mdv::modbus::kModbusPollFailuresBeforeOffline;
         ++failure) {
        const auto failed = driver.ProcessNext();
        Require(
            failed.outcome == mdv::DriverOutcome::InvalidResponse,
            "malformed batch was not rejected");
        Require(
            failed.error.find("stage=semantic-read") != std::string::npos &&
                failed.error.find("registers=40028..40029") !=
                    std::string::npos &&
                failed.error.find(
                    "consecutive-poll-failures=" +
                    std::to_string(failure) + "/3") != std::string::npos,
            "poll failure diagnostic lacks stage, register range or counter");
        Require(
            driver.DeviceStateByAddress(1U).online,
            "device went offline before the poll failure threshold");
    }

    const auto thresholdFailure = driver.ProcessNext();
    Require(
        thresholdFailure.outcome == mdv::DriverOutcome::InvalidResponse &&
            thresholdFailure.error.find("device marked offline") !=
                std::string::npos,
        "third malformed batch did not reach the offline threshold");

    const auto after = driver.DeviceStateByAddress(1U);
    Require(!after.online, "third malformed batch left the device online");
    Require(after.hasState, "malformed batch erased the prior factual snapshot");
    Require(after.power == factual.power, "malformed batch changed factual Power");
    Require(
        after.roomTemperature == factual.roomTemperature,
        "malformed batch changed factual room temperature");
    Require(
        after.alarmCode == factual.alarmCode,
        "malformed batch changed factual AlarmCode");

    transport.malformedBatch = false;
    Require(
        driver.ProcessNext().outcome == mdv::DriverOutcome::Success &&
            driver.DeviceStateByAddress(1U).online,
        "first complete successful poll did not restore the device online");

    transport.malformedBatch = true;
    const auto afterRecoveryFailure = driver.ProcessNext();
    Require(
        afterRecoveryFailure.error.find("consecutive-poll-failures=1/3") !=
                std::string::npos &&
            driver.DeviceStateByAddress(1U).online,
        "successful poll did not reset the consecutive failure counter");
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
    Require(after.online, "one failed snapshot marked device offline");
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

void TestModeSpeedAndSetTemperatureWritesAreConfirmed()
{
    auto profile = ProductionProfile();
    PowerTransport transport;
    mdv::modbus::ModbusDriver driver({1U}, profile, transport);
    InitializeOne(driver);
    transport.requests.clear();

    const auto verifyCommand = [&](
                                   mdv::DriverControl control,
                                   mdv::DriverCommandValue value,
                                   std::uint16_t writeAddress,
                                   std::uint16_t rawValue,
                                   std::uint16_t readAddress) {
        driver.ApplyCommand(mdv::DriverCommand{
            .address = 1U,
            .control = control,
            .value = std::move(value),
        });

        auto requestOffset = transport.requests.size();
        auto write = driver.ProcessNext();
        if (write.operation == mdv::DriverOperation::PollRead) {
            requestOffset = transport.requests.size();
            write = driver.ProcessNext();
        }
        if (write.operation != mdv::DriverOperation::SetState ||
            write.outcome != mdv::DriverOutcome::Success) {
            throw std::runtime_error(
                "semantic FC10 write failed: " + write.error);
        }
        Require(
            RequestFunction(transport.requests[requestOffset]) == 0x10U &&
                RequestAddress(transport.requests[requestOffset]) == writeAddress &&
                RequestWriteValue(transport.requests[requestOffset]) == rawValue,
            "semantic FC10 request mismatch");

        const auto confirmation = driver.ProcessNext();
        Require(
            confirmation.operation == mdv::DriverOperation::ConfirmRead &&
                confirmation.outcome == mdv::DriverOutcome::Success,
            "semantic read-back confirmation failed");
        Require(
            RequestFunction(transport.requests[requestOffset + 1U]) == 0x03U &&
                RequestAddress(transport.requests[requestOffset + 1U]) == readAddress,
            "semantic confirmation register mismatch");
    };

    verifyCommand(
        mdv::DriverControl::Mode,
        mdv::HvacMode::Cool,
        40079U,
        2U,
        40029U);
    Require(
        driver.DeviceStateByAddress(1U).mode == mdv::HvacMode::Cool,
        "confirmed Mode did not become factual");

    verifyCommand(
        mdv::DriverControl::FanSpeed,
        mdv::HvacFanSpeed::Low,
        40080U,
        8U,
        40030U);
    Require(
        driver.DeviceStateByAddress(1U).fanSpeed == mdv::HvacFanSpeed::Low,
        "confirmed FanSpeed did not become factual");

    verifyCommand(
        mdv::DriverControl::SetTemperature,
        26.0,
        40081U,
        26U,
        40031U);
    Require(
        driver.DeviceStateByAddress(1U).setTemperature == 26.0,
        "confirmed SetTemperature did not become factual");
    Require(!driver.HasQueuedWork(), "confirmed semantic commands remained queued");
}

void TestIndependentControlsCanBeQueuedTogether()
{
    auto profile = ProductionProfile();
    PowerTransport transport;
    mdv::modbus::ModbusDriver driver({1U}, profile, transport);
    InitializeOne(driver);
    transport.requests.clear();

    for (const auto& command : {
             mdv::DriverCommand{
                 .address = 1U,
                 .control = mdv::DriverControl::Mode,
                 .value = mdv::HvacMode::Heat,
             },
             mdv::DriverCommand{
                 .address = 1U,
                 .control = mdv::DriverControl::FanSpeed,
                 .value = mdv::HvacFanSpeed::High,
             },
             mdv::DriverCommand{
                 .address = 1U,
                 .control = mdv::DriverControl::SetTemperature,
                 .value = 27.0,
             },
             mdv::DriverCommand{
                 .address = 1U,
                 .control = mdv::DriverControl::Power,
                 .value = false,
             }}) {
        driver.ApplyCommand(command);
    }

    for (std::size_t operation = 0U;
         operation < 16U && driver.HasQueuedWork();
         ++operation) {
        static_cast<void>(driver.ProcessNext());
    }

    Require(!driver.HasQueuedWork(),
            "simultaneous semantic commands did not finish");
    const auto state = driver.DeviceStateByAddress(1U);
    Require(!state.power, "queued Power did not become factual");
    Require(state.mode == mdv::HvacMode::Heat,
            "queued Mode did not become factual");
    Require(state.fanSpeed == mdv::HvacFanSpeed::High,
            "queued FanSpeed did not become factual");
    Require(state.setTemperature == 27.0,
            "queued SetTemperature did not become factual");

    for (const auto address : {40078U, 40079U, 40080U, 40081U}) {
        const auto count = std::count_if(
            transport.requests.begin(),
            transport.requests.end(),
            [address](const auto& request) {
                return RequestFunction(request) == 0x10U &&
                    RequestAddress(request) == address;
            });
        Require(count == 1,
                "queued semantic control was not written exactly once");
    }
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
            result.error.find("stage=write") != std::string::npos &&
                result.error.find("register=40078") != std::string::npos,
            "write timeout diagnostic lacks stage or register");
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
            result.error.find("stage=confirmation-read") !=
                    std::string::npos &&
                result.error.find("register=40028") != std::string::npos,
            "confirmation timeout diagnostic lacks stage or register");
        Require(
            driver.HasQueuedWork() ==
                (attempt < mdv::modbus::kMaxModbusConfirmationAttempts),
            "confirmation retry budget mismatch");
    }

    Require(
        driver.DeviceStateByAddress(1U).online,
        "confirmation failures changed full-poll availability");
    Require(
        driver.DeviceStateByAddress(1U).power,
        "failed confirmations changed factual Power");
}

void TestInvalidConfirmationPreservesAvailability()
{
    auto profile = ProductionProfile();
    InvalidConfirmationTransport transport;
    mdv::modbus::ModbusDriver driver({1U}, profile, transport);
    InitializeOne(driver);
    transport.requests.clear();

    driver.ApplyCommand(mdv::DriverCommand{
        .address = 1U,
        .control = mdv::DriverControl::Power,
        .value = false,
    });

    const auto write = driver.ProcessNext();
    Require(
        write.operation == mdv::DriverOperation::SetState &&
            write.outcome == mdv::DriverOutcome::Success,
        "FC10 before invalid confirmation failed");

    const auto confirmation = driver.ProcessNext();
    Require(
        confirmation.operation == mdv::DriverOperation::ConfirmRead &&
            confirmation.outcome == mdv::DriverOutcome::InvalidResponse,
        "invalid Power confirmation classification is wrong");

    const auto state = driver.DeviceStateByAddress(1U);
    Require(state.online, "invalid Power confirmation marked device offline");
    Require(state.hasState, "invalid Power confirmation erased factual state");
    Require(state.power, "invalid Power confirmation changed factual Power");
    Require(!driver.HasQueuedWork(),
            "invalid Power confirmation remained queued");

    Require(
        confirmation.error.find("stage=confirmation-decode") !=
                std::string::npos &&
            confirmation.error.find("register=40028") != std::string::npos,
        "invalid confirmation diagnostic lacks stage or register");

    driver.ApplyCommand(mdv::DriverCommand{
        .address = 1U,
        .control = mdv::DriverControl::Power,
        .value = false,
    });
    Require(
        driver.HasQueuedWork(),
        "available device rejected a command after invalid confirmation");
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

    bool unsupportedRejected = false;
    try {
        driver.ApplyCommand(mdv::DriverCommand{
            .address = 1U,
            .control = mdv::DriverControl::Blinds,
            .value = false,
        });
    }
    catch (const std::invalid_argument&) {
        unsupportedRejected = true;
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

    Require(unsupportedRejected, "unsupported Blinds command was accepted");
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
        metrics.semanticTransactionsPerCycle == 12U,
        "driver semantic baseline is wrong");
    Require(
        metrics.totalTransactionsPerCycle == 14U,
        "driver total transaction baseline is wrong");
    Require(
        metrics.registersRequestedPerCycle == 14U,
        "driver register baseline is wrong");
    Require(
        metrics.optimizedSemanticTransactionsPerCycle == 4U &&
            metrics.optimizedTotalTransactionsPerCycle == 6U &&
            metrics.optimizedRegistersRequestedPerCycle == 12U &&
            metrics.reusedSemanticReadsPerCycle == 2U &&
            metrics.savedTransactionsPerCycle == 8U,
        "driver production optimization metrics are wrong");
    Require(
        transport.requests.empty(),
        "building the resolved poll plan generated Modbus traffic");
}

void TestConfiguredRetryAndFairnessPolicy()
{
    auto profile = ProductionProfile();

    WriteFailureTransport writeTransport;
    mdv::modbus::ModbusDriver writeDriver(
        {1U},
        profile,
        writeTransport,
        mdv::modbus::ModbusDriverPolicy{
            .maxWriteAttempts = 2U,
            .maxConfirmationAttempts = 3U,
            .maxPriorityOperationsBeforePoll = 4U,
        });
    InitializeOne(writeDriver);
    writeTransport.requests.clear();
    writeDriver.ApplyCommand(mdv::DriverCommand{
        .address = 1U,
        .control = mdv::DriverControl::Power,
        .value = false,
    });
    static_cast<void>(writeDriver.ProcessNext());
    Require(writeDriver.HasQueuedWork(), "custom write retry ended too early");
    static_cast<void>(writeDriver.ProcessNext());
    Require(!writeDriver.HasQueuedWork(), "custom write retry exceeded its budget");
    Require(writeTransport.requests.size() == 2U, "custom write retry count mismatch");

    PowerTransport fairnessTransport;
    fairnessTransport.applyWrites = false;
    mdv::modbus::ModbusDriver fairnessDriver(
        {1U},
        profile,
        fairnessTransport,
        mdv::modbus::ModbusDriverPolicy{
            .maxWriteAttempts = 3U,
            .maxConfirmationAttempts = 3U,
            .maxPriorityOperationsBeforePoll = 1U,
        });
    InitializeOne(fairnessDriver);
    fairnessTransport.requests.clear();
    fairnessDriver.ApplyCommand(mdv::DriverCommand{
        .address = 1U,
        .control = mdv::DriverControl::Power,
        .value = false,
    });
    const auto write = fairnessDriver.ProcessNext();
    Require(write.operation == mdv::DriverOperation::SetState,
            "custom priority burst did not start with queued write");
    const auto poll = fairnessDriver.ProcessNext();
    Require(poll.operation == mdv::DriverOperation::PollRead,
            "custom priority burst did not yield to polling");
}

void TestInvalidDriverPolicyRejected()
{
    auto profile = ProductionProfile();
    ScriptedReadTransport transport;

    for (const auto policy : {
             mdv::modbus::ModbusDriverPolicy{
                 .maxWriteAttempts = 0U,
                 .maxConfirmationAttempts = 3U,
                 .maxPriorityOperationsBeforePoll = 4U,
             },
             mdv::modbus::ModbusDriverPolicy{
                 .maxWriteAttempts = 3U,
                 .maxConfirmationAttempts = 11U,
                 .maxPriorityOperationsBeforePoll = 4U,
             },
             mdv::modbus::ModbusDriverPolicy{
                 .maxWriteAttempts = 3U,
                 .maxConfirmationAttempts = 3U,
                 .maxPriorityOperationsBeforePoll = 0U,
             }}) {
        bool rejected = false;
        try {
            mdv::modbus::ModbusDriver driver({1U}, profile, transport, policy);
            static_cast<void>(driver);
        }
        catch (const std::invalid_argument&) {
            rejected = true;
        }
        Require(rejected, "invalid Modbus driver policy was accepted");
    }
    Require(transport.requests.empty(), "policy validation generated bus traffic");
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
        TestModeSpeedAndSetTemperatureWritesAreConfirmed();
        TestIndependentControlsCanBeQueuedTogether();
        TestWriteTimeoutRetriesAreBounded();
        TestConfirmationTimeoutRetriesAreBounded();
        TestInvalidConfirmationPreservesAvailability();
        TestConfirmationMismatchRetriesWrite();
        TestPriorityWorkCannotStarvePolling();
        TestNewerCommandCancelsStaleWork();
        TestUnsupportedCommandsGenerateNoTraffic();
        TestResolvedPollPlanBaselineIsExposed();
        TestConfiguredRetryAndFairnessPolicy();
        TestInvalidDriverPolicyRejected();
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
