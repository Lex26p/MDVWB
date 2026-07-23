#include "MDVWB.h"
#include "mdv_device.h"
#include "mdv_driver.h"
#include "mdv_mqtt.h"
#include "mdv_mosquitto.h"
#include "mdv_protocol.h"
#include "mdv_serial.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using mdv::RequestFrame;
using mdv::ResponseFrame;

bool Check(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

template <std::size_t Size>
bool CheckFrame(
    const std::array<std::uint8_t, Size>& actual,
    const std::array<std::uint8_t, Size>& expected,
    std::string_view message)
{
    return Check(actual == expected, message);
}

mdv::DeviceState MakeReadState(
    std::uint8_t address,
    bool power,
    std::optional<mdv::Mode> mode,
    std::optional<mdv::FanSpeed> speed,
    std::uint8_t setTemperature,
    std::uint8_t additionalFunctions = 0)
{
    mdv::DeviceState state;
    state.command = mdv::Command::Read;
    state.address = address;
    state.masterId = 0;
    state.power = power;
    state.mode = mode;
    state.activeMode = mode;
    state.fanSpeed = speed;
    state.activeFanSpeed = speed;
    state.setTemperature = setTemperature;
    state.additionalFunctions = additionalFunctions;
    state.blinds = (additionalFunctions & 0x04) != 0;
    return state;
}

bool TestReadRequests()
{
    constexpr RequestFrame expectedAddress0{
        0xAA, 0xC0, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x81, 0x55};
    constexpr RequestFrame expectedAddress1{
        0xAA, 0xC0, 0x01, 0x00, 0x80, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x55};

    return CheckFrame(mdv::BuildReadRequest(0), expectedAddress0, "C0 address 0 frame") &&
        CheckFrame(mdv::BuildReadRequest(1), expectedAddress1, "C0 address 1 frame");
}

bool TestSetRequests()
{
    const mdv::SetState onState{
        .power = true,
        .mode = mdv::Mode::Auto,
        .fanSpeed = mdv::FanSpeed::Auto,
        .setTemperature = 23,
        .additionalFunctions = 0,
    };
    const mdv::SetState offState{
        .power = false,
        .mode = mdv::Mode::Auto,
        .fanSpeed = mdv::FanSpeed::Auto,
        .setTemperature = 23,
        .additionalFunctions = 0,
    };

    constexpr RequestFrame expectedOn{
        0xAA, 0xC3, 0x18, 0x00, 0x80, 0x00, 0x90, 0x80,
        0x17, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x42, 0x55};
    constexpr RequestFrame expectedOff{
        0xAA, 0xC3, 0x18, 0x00, 0x80, 0x00, 0x10, 0x80,
        0x17, 0x00, 0x00, 0x00, 0x00, 0x3C, 0xC2, 0x55};

    return CheckFrame(mdv::BuildSetRequest(0x18, onState), expectedOn, "C3 power on frame") &&
        CheckFrame(mdv::BuildSetRequest(0x18, offState), expectedOff, "C3 power off frame");
}

bool TestResponseParsing()
{
    constexpr ResponseFrame response{
        0xAA, 0xC0, 0x80, 0x00, 0x00, 0x00, 0xE0, 0x14,
        0x88, 0x84, 0x17, 0x58, 0x3C, 0x4C, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xC8, 0x55};

    const auto parsed = mdv::ParseResponse(response, 0);
    return Check(parsed.ok, parsed.error) &&
        Check(parsed.state.power, "response Power") &&
        Check(parsed.state.mode == mdv::Mode::Cool, "response Mode Cool") &&
        Check(parsed.state.fanSpeed == mdv::FanSpeed::Auto, "response Speed Auto") &&
        Check(parsed.state.activeFanSpeed == mdv::FanSpeed::Low, "response active Speed Low") &&
        Check(parsed.state.setTemperature == 23, "response SetTemp") &&
        Check(parsed.state.roomTemperature.has_value(), "response room temperature exists") &&
        Check(std::abs(*parsed.state.roomTemperature - 24.0) < 0.001, "response room temperature value") &&
        Check(!parsed.state.blinds, "response Blinds off") &&
        Check(parsed.state.errorsE0E7 == 0 && parsed.state.errorsE8EF == 0, "response errors");
}

bool TestAutoModeParsing()
{
    constexpr ResponseFrame response{
        0xAA, 0xC0, 0x80, 0x00, 0x12, 0x00, 0xE0, 0x14,
        0x98, 0x84, 0x16, 0x54, 0x3E, 0x48, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x05, 0xFF, 0xFF, 0xA1, 0x55};

    const auto parsed = mdv::ParseResponse(response, 0x12);
    return Check(parsed.ok, parsed.error) &&
        Check(parsed.state.mode == mdv::Mode::Auto, "Auto mode selected") &&
        Check(parsed.state.activeMode == mdv::Mode::Cool, "Auto active mode Cool") &&
        Check(parsed.state.fanSpeed == mdv::FanSpeed::Auto, "Auto speed selected") &&
        Check(parsed.state.activeFanSpeed == mdv::FanSpeed::Low, "Auto active speed Low");
}

bool TestStreamCollector()
{
    constexpr ResponseFrame response{
        0xAA, 0xC0, 0x80, 0x00, 0x00, 0x00, 0xE0, 0x14,
        0x88, 0x84, 0x17, 0x58, 0x3C, 0x4C, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xC8, 0x55};

    std::vector<std::uint8_t> stream{0xFE, 0x00, 0xFE};
    stream.insert(stream.end(), response.begin(), response.end());
    stream.push_back(0x00);
    stream.push_back(0xFE);

    mdv::ResponseFrameCollector collector;
    std::optional<ResponseFrame> extracted;
    for (const auto byte : stream) {
        if (auto frame = collector.Push(byte); frame.has_value()) {
            extracted = frame;
        }
    }

    return Check(extracted.has_value(), "stream frame extraction") &&
        CheckFrame(*extracted, response, "stream extracted frame content");
}

bool TestCachedSetFrame()
{
    mdv::DeviceContext device(0x18);
    auto state = MakeReadState(
        0x18, true, mdv::Mode::Auto, mdv::FanSpeed::Auto, 23, 0x04);
    device.SynchronizeReadState(state);

    const auto& frame = device.CachedSetFrame();
    return Check(device.IsInitialized(), "device cache initialized") &&
        Check(frame[1] == 0xC3 && frame[2] == 0x18, "cached C3 address") &&
        Check(frame[6] == 0x90, "cached Power and Auto mode") &&
        Check(frame[7] == 0x80, "cached Auto speed") &&
        Check(frame[8] == 23, "cached SetTemp") &&
        Check(frame[9] == 0x04, "cached Blinds") &&
        Check(mdv::HasValidRequestChecksum(frame), "cached checksum");
}

bool TestSingleFieldModification()
{
    mdv::DeviceContext device(1);
    device.SynchronizeReadState(MakeReadState(
        1, true, mdv::Mode::Cool, mdv::FanSpeed::Auto, 23));

    const auto before = device.CachedSetFrame();
    device.ApplySetTemperature(24);
    const auto after = device.CachedSetFrame();

    bool onlyExpectedBytesChanged = true;
    for (std::size_t index = 0; index < after.size(); ++index) {
        if (index != 8 && index != 14 && before[index] != after[index]) {
            onlyExpectedBytesChanged = false;
        }
    }

    return Check(onlyExpectedBytesChanged, "SetTemp changes only payload byte and checksum") &&
        Check(after[8] == 24, "cached SetTemp updated") &&
        Check(device.HasPendingField(mdv::PendingField::SetTemperature), "SetTemp pending") &&
        Check(device.IsSetCommandQueued(), "C3 queued") &&
        Check(mdv::HasValidRequestChecksum(after), "updated checksum");
}

bool TestOldReadDoesNotOverwriteCommand()
{
    mdv::DeviceContext device(2);
    auto state = MakeReadState(2, true, mdv::Mode::Cool, mdv::FanSpeed::Low, 23);
    device.SynchronizeReadState(state);
    device.ApplySetTemperature(24);

    device.SynchronizeReadState(state); // fan coil still reports the old value
    const bool oldValuePreserved = device.CachedSetFrame()[8] == 24 &&
        device.HasPendingField(mdv::PendingField::SetTemperature);

    state.setTemperature = 24;
    device.SynchronizeReadState(state);

    return Check(oldValuePreserved, "old C0 does not overwrite desired SetTemp") &&
        Check(!device.HasPendingField(mdv::PendingField::SetTemperature), "new C0 confirms SetTemp");
}

bool TestLocalPanelSynchronizesFreeFields()
{
    mdv::DeviceContext device(3);
    device.SynchronizeReadState(MakeReadState(
        3, false, mdv::Mode::Auto, mdv::FanSpeed::Auto, 21));

    device.SynchronizeReadState(MakeReadState(
        3, true, mdv::Mode::Heat, mdv::FanSpeed::Medium, 25, 0x04));
    const auto& frame = device.CachedSetFrame();

    return Check(frame[6] == 0x84, "local Power and Heat synchronized") &&
        Check(frame[7] == 0x02, "local Speed synchronized") &&
        Check(frame[8] == 25, "local SetTemp synchronized") &&
        Check(frame[9] == 0x04, "local Blinds synchronized") &&
        Check(device.PendingFields() == mdv::PendingField::None, "local changes are not pending");
}

bool TestSafeFallbackAndRevision()
{
    mdv::DeviceContext device(4);
    device.SynchronizeReadState(MakeReadState(4, false, std::nullopt, std::nullopt, 0));
    const auto& fallback = device.CachedSetFrame();
    const bool safeFallback = fallback[6] == 0x10 && fallback[7] == 0x80 && fallback[8] == 21;

    device.ApplyPower(true);
    const auto sending = device.PrepareSetFrameForSend();
    const bool dequeued = !device.IsSetCommandQueued();

    device.ApplySetTemperature(22);
    device.FinishSetFrameSend(sending.revision);

    return Check(safeFallback, "powered-off zero state gets safe C3 fallback") &&
        Check(dequeued, "prepared C3 leaves queue") &&
        Check(device.IsSetCommandQueued(), "newer MQTT change requeues C3") &&
        Check(device.DesiredRevision() > sending.revision, "desired revision advanced");
}

bool TestUnsafeUseIsRejected()
{
    bool beforeReadRejected = false;
    try {
        mdv::DeviceContext device(5);
        device.ApplyPower(true);
    }
    catch (const std::logic_error&) {
        beforeReadRejected = true;
    }

    bool c3SyncRejected = false;
    try {
        mdv::DeviceContext device(6);
        auto state = MakeReadState(6, false, mdv::Mode::Auto, mdv::FanSpeed::Auto, 21);
        state.command = mdv::Command::Set;
        device.SynchronizeReadState(state);
    }
    catch (const std::invalid_argument&) {
        c3SyncRejected = true;
    }

    bool badTemperatureRejected = false;
    try {
        auto state = mdv::SetState{};
        state.setTemperature = 33;
        static_cast<void>(mdv::BuildSetRequest(1, state));
    }
    catch (const std::invalid_argument&) {
        badTemperatureRejected = true;
    }

    constexpr ResponseFrame validResponse{
        0xAA, 0xC0, 0x80, 0x00, 0x00, 0x00, 0xE0, 0x14,
        0x88, 0x84, 0x17, 0x58, 0x3C, 0x4C, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xC8, 0x55};
    auto corrupted = validResponse;
    corrupted[11] ^= 0x01;

    return Check(beforeReadRejected, "C3 forbidden before first C0") &&
        Check(c3SyncRejected, "C3 response cannot synchronize cache") &&
        Check(badTemperatureRejected, "unsafe SetTemp rejected") &&
        Check(!mdv::ParseResponse(corrupted, 0).ok, "invalid response checksum rejected");
}


bool TestWireRequest()
{
    constexpr RequestFrame request{
        0xAA, 0xC0, 0x01, 0x00, 0x80, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x55};
    constexpr mdv::WireRequest expected{
        0xFE, 0xAA, 0xC0, 0x01, 0x00, 0x80, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x55};

    return CheckFrame(mdv::BuildWireRequest(request), expected, "wire request FE prefix");
}

bool TestTransactionTiming()
{
    const mdv::TimingSettings settings{
        .transactionPeriod = std::chrono::milliseconds(150),
        .responseTimeout = std::chrono::milliseconds(130),
    };
    const mdv::TransactionPacer pacer(settings);
    const auto start = std::chrono::steady_clock::time_point{};

    return Check(pacer.Settings().transactionPeriod == std::chrono::milliseconds(150),
                 "transaction period is 150 ms") &&
        Check(pacer.Settings().responseTimeout == std::chrono::milliseconds(130),
              "response timeout is 130 ms") &&
        Check(pacer.ResponseDeadline(start) == start + std::chrono::milliseconds(130),
              "response deadline belongs to transaction slot") &&
        Check(pacer.NextAllowedStart(start) == start + std::chrono::milliseconds(150),
              "next request starts after full 150 ms period");
}

bool TestInvalidTimingRejected()
{
    bool equalTimeoutRejected = false;
    try {
        static_cast<void>(mdv::TransactionPacer({
            .transactionPeriod = std::chrono::milliseconds(150),
            .responseTimeout = std::chrono::milliseconds(150),
        }));
    }
    catch (const std::invalid_argument&) {
        equalTimeoutRejected = true;
    }

    bool longerTimeoutRejected = false;
    try {
        static_cast<void>(mdv::TransactionPacer({
            .transactionPeriod = std::chrono::milliseconds(150),
            .responseTimeout = std::chrono::milliseconds(200),
        }));
    }
    catch (const std::invalid_argument&) {
        longerTimeoutRejected = true;
    }

    return Check(equalTimeoutRejected, "response timeout cannot consume the full slot") &&
        Check(longerTimeoutRejected, "response timeout cannot exceed the slot");
}

bool TestPortNameNormalization()
{
#ifdef _WIN32
    return Check(mdv::SerialPort::NormalizePortName("COM3") == R"(\\.\COM3)",
                 "Windows COM port normalization") &&
        Check(mdv::SerialPort::NormalizePortName(R"(\\.\COM12)") == R"(\\.\COM12)",
              "normalized Windows COM port remains unchanged");
#else
    return Check(mdv::SerialPort::NormalizePortName("/dev/ttyRS485-1") ==
                     "/dev/ttyRS485-1",
                 "Linux serial path remains unchanged");
#endif
}


class ScriptedTransport final : public mdv::ITransactionTransport {
public:
    explicit ScriptedTransport(std::vector<mdv::TransactionResult> results)
        : results_(std::move(results))
    {
    }

    mdv::TransactionResult Execute(const RequestFrame& request) override
    {
        requests.push_back(request);
        if (nextResult_ >= results_.size()) {
            return mdv::TransactionResult{
                .status = mdv::TransactionStatus::IoError,
                .response = std::nullopt,
                .error = "scripted transport has no result",
            };
        }
        return results_[nextResult_++];
    }

    std::vector<RequestFrame> requests;

private:
    std::vector<mdv::TransactionResult> results_;
    std::size_t nextResult_ = 0;
};

ResponseFrame MakeDriverResponse(
    std::uint8_t address,
    mdv::Command command = mdv::Command::Read,
    std::uint8_t modeByte = 0x88,
    std::uint8_t speedByte = 0x84,
    std::uint8_t setTemperature = 23,
    std::uint8_t additionalFunctions = 0,
    std::uint8_t roomTemperatureRaw = 0x58,
    std::uint8_t errorsE0E7 = 0,
    std::uint8_t errorsE8EF = 0)
{
    ResponseFrame response{
        0xAA, 0xC0, 0x80, 0x00, 0x00, 0x00, 0xE0, 0x14,
        0x88, 0x84, 0x17, 0x58, 0x3C, 0x4C, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x55};

    response[1] = static_cast<std::uint8_t>(command);
    response[4] = address;
    response[8] = modeByte;
    response[9] = speedByte;
    response[10] = setTemperature;
    response[11] = roomTemperatureRaw;
    response[20] = additionalFunctions;
    response[22] = errorsE0E7;
    response[23] = errorsE8EF;

    std::uint8_t sum = 0;
    for (std::size_t index = 1; index <= 29; ++index) {
        sum = static_cast<std::uint8_t>(sum + response[index]);
    }
    response[30] = static_cast<std::uint8_t>(0U - sum);
    return response;
}

mdv::TransactionResult SuccessfulTransaction(ResponseFrame response)
{
    return mdv::TransactionResult{
        .status = mdv::TransactionStatus::Success,
        .response = std::move(response),
        .error = {},
    };
}

bool TestRoundRobinPolling()
{
    ScriptedTransport transport({
        SuccessfulTransaction(MakeDriverResponse(1)),
        SuccessfulTransaction(MakeDriverResponse(2)),
        SuccessfulTransaction(MakeDriverResponse(3)),
        SuccessfulTransaction(MakeDriverResponse(1, mdv::Command::Read, 0x88, 0x84, 24)),
    });
    mdv::MdvDriver driver({1, 2, 3}, transport);

    const auto first = driver.ProcessNext();
    const auto second = driver.ProcessNext();
    const auto third = driver.ProcessNext();
    const auto fourth = driver.ProcessNext();

    const bool requestOrder = transport.requests.size() == 4 &&
        transport.requests[0][2] == 1 &&
        transport.requests[1][2] == 2 &&
        transport.requests[2][2] == 3 &&
        transport.requests[3][2] == 1;

    return Check(first.operation == mdv::DriverOperation::PollRead &&
                     first.outcome == mdv::DriverOutcome::Success && first.address == 1,
                 "round-robin first device") &&
        Check(second.outcome == mdv::DriverOutcome::Success && second.address == 2,
              "round-robin second device") &&
        Check(third.outcome == mdv::DriverOutcome::Success && third.address == 3,
              "round-robin third device") &&
        Check(fourth.outcome == mdv::DriverOutcome::Success && fourth.address == 1,
              "round-robin wraps to first device") &&
        Check(requestOrder, "round-robin C0 request order") &&
        Check(driver.NextPollAddress() == 2, "next round-robin address") &&
        Check(driver.DeviceByAddress(1).device.IsInitialized(),
              "first valid C0 initializes cached C3") &&
        Check(driver.DeviceByAddress(1).device.ActualState().setTemperature == 24,
              "later C0 updates actual state") &&
        Check(driver.DeviceByAddress(1).successfulReads == 2,
              "successful read counter") &&
        Check(driver.DeviceByAddress(1).online, "successful device is online");
}

bool TestPollingContinuesAfterTimeout()
{
    ScriptedTransport transport({
        mdv::TransactionResult{
            .status = mdv::TransactionStatus::Timeout,
            .response = std::nullopt,
            .error = "timeout",
        },
        SuccessfulTransaction(MakeDriverResponse(5)),
        SuccessfulTransaction(MakeDriverResponse(4)),
    });
    mdv::MdvDriver driver({4, 5}, transport);

    const auto timeout = driver.ProcessNext();
    const auto nextDevice = driver.ProcessNext();
    const auto recovered = driver.ProcessNext();
    const auto& device4 = driver.DeviceByAddress(4);

    return Check(timeout.outcome == mdv::DriverOutcome::Timeout && timeout.address == 4,
                 "poll timeout result") &&
        Check(nextDevice.outcome == mdv::DriverOutcome::Success && nextDevice.address == 5,
              "timeout does not stop round-robin polling") &&
        Check(recovered.outcome == mdv::DriverOutcome::Success && recovered.address == 4,
              "timed-out device is polled again") &&
        Check(device4.online, "device returns online after valid response") &&
        Check(device4.failedReads == 1 && device4.successfulReads == 1,
              "communication counters survive recovery") &&
        Check(device4.consecutiveReadFailures == 0,
              "recovery clears consecutive failures");
}

bool TestInvalidPollingResponses()
{
    auto corrupted = MakeDriverResponse(6);
    corrupted[11] ^= 0x01;

    ScriptedTransport transport({
        SuccessfulTransaction(corrupted),
        SuccessfulTransaction(MakeDriverResponse(6, mdv::Command::Set)),
    });
    mdv::MdvDriver driver({6}, transport);

    const auto badChecksum = driver.ProcessNext();
    const auto wrongCommand = driver.ProcessNext();
    const auto& runtime = driver.DeviceByAddress(6);

    return Check(badChecksum.outcome == mdv::DriverOutcome::InvalidResponse,
                 "poll rejects bad response checksum") &&
        Check(wrongCommand.outcome == mdv::DriverOutcome::InvalidResponse,
              "poll rejects C3 response while expecting C0") &&
        Check(!runtime.device.IsInitialized(),
              "invalid polling responses do not initialize cached frame") &&
        Check(runtime.failedReads == 2 && !runtime.online,
              "invalid responses update communication state");
}

bool TestPollingAddressValidation()
{
    ScriptedTransport transport({});

    bool emptyRejected = false;
    try {
        static_cast<void>(mdv::MdvDriver({}, transport));
    }
    catch (const std::invalid_argument&) {
        emptyRejected = true;
    }

    bool duplicateRejected = false;
    try {
        static_cast<void>(mdv::MdvDriver({1, 1}, transport));
    }
    catch (const std::invalid_argument&) {
        duplicateRejected = true;
    }

    bool invalidAddressRejected = false;
    try {
        static_cast<void>(mdv::MdvDriver({0x40}, transport));
    }
    catch (const std::out_of_range&) {
        invalidAddressRejected = true;
    }

    return Check(emptyRejected, "empty polling list rejected") &&
        Check(duplicateRejected, "duplicate polling address rejected") &&
        Check(invalidAddressRejected, "out-of-range polling address rejected");
}

bool TestSetQueueAndConfirmation()
{
    ScriptedTransport transport({
        SuccessfulTransaction(MakeDriverResponse(1)),
        SuccessfulTransaction(MakeDriverResponse(2)),
        SuccessfulTransaction(MakeDriverResponse(2, mdv::Command::Set)),
        SuccessfulTransaction(MakeDriverResponse(2)),
        SuccessfulTransaction(MakeDriverResponse(2, mdv::Command::Set)),
        SuccessfulTransaction(MakeDriverResponse(
            2, mdv::Command::Read, 0x88, 0x84, 24)),
        SuccessfulTransaction(MakeDriverResponse(1)),
    });
    mdv::MdvDriver driver({1, 2}, transport);

    static_cast<void>(driver.ProcessNext());
    static_cast<void>(driver.ProcessNext());
    driver.SetTemperature(2, 24);

    const auto set1 = driver.ProcessNext();
    const auto confirmOld = driver.ProcessNext();
    const bool oldStatePreserved =
        driver.DeviceByAddress(2).device.CachedSetFrame()[8] == 24 &&
        driver.DeviceByAddress(2).device.HasPendingField(
            mdv::PendingField::SetTemperature);
    const auto set2 = driver.ProcessNext();
    const auto confirmNew = driver.ProcessNext();
    const auto normalPoll = driver.ProcessNext();

    const bool order = transport.requests.size() == 7 &&
        transport.requests[2][1] == static_cast<std::uint8_t>(mdv::Command::Set) &&
        transport.requests[2][2] == 2 && transport.requests[2][8] == 24 &&
        transport.requests[3][1] == static_cast<std::uint8_t>(mdv::Command::Read) &&
        transport.requests[3][2] == 2 &&
        transport.requests[4][1] == static_cast<std::uint8_t>(mdv::Command::Set) &&
        transport.requests[5][1] == static_cast<std::uint8_t>(mdv::Command::Read) &&
        transport.requests[6][2] == 1;

    return Check(set1.operation == mdv::DriverOperation::SetState &&
                     set1.outcome == mdv::DriverOutcome::Success,
                 "queued C3 has priority over ordinary poll") &&
        Check(confirmOld.operation == mdv::DriverOperation::ConfirmRead,
              "C3 is followed by confirmation C0") &&
        Check(oldStatePreserved, "old confirmation does not erase desired value") &&
        Check(set2.operation == mdv::DriverOperation::SetState,
              "old confirmation requeues cached C3") &&
        Check(confirmNew.operation == mdv::DriverOperation::ConfirmRead &&
                  confirmNew.outcome == mdv::DriverOutcome::Success,
              "new value is confirmed by C0") &&
        Check(!driver.DeviceByAddress(2).device.HasPendingField(
                  mdv::PendingField::SetTemperature),
              "confirmed field leaves pending mask") &&
        Check(normalPoll.operation == mdv::DriverOperation::PollRead &&
                  normalPoll.address == 1,
              "round-robin resumes after command sequence") &&
        Check(order, "C3 and C0 transaction order");
}

bool TestMultipleCommandsMergeIntoOneFrame()
{
    ScriptedTransport transport({
        SuccessfulTransaction(MakeDriverResponse(
            3, mdv::Command::Read, 0x10, 0x80, 21)),
        SuccessfulTransaction(MakeDriverResponse(
            3, mdv::Command::Set, 0x10, 0x80, 21)),
        SuccessfulTransaction(MakeDriverResponse(
            3, mdv::Command::Read, 0x84, 0x02, 25, 0x04)),
    });
    mdv::MdvDriver driver({3}, transport);

    static_cast<void>(driver.ProcessNext());
    driver.SetPower(3, true);
    driver.SetMode(3, mdv::Mode::Heat);
    driver.SetFanSpeed(3, mdv::FanSpeed::Medium);
    driver.SetTemperature(3, 25);
    driver.SetBlinds(3, true);

    const auto set = driver.ProcessNext();
    const auto confirm = driver.ProcessNext();
    const auto& frame = transport.requests[1];

    return Check(set.operation == mdv::DriverOperation::SetState,
                 "merged command sends C3") &&
        Check(frame[6] == 0x84, "merged Power and Heat") &&
        Check(frame[7] == 0x02, "merged Medium speed") &&
        Check(frame[8] == 25, "merged SetTemp") &&
        Check(frame[9] == 0x04, "merged Blinds") &&
        Check(mdv::HasValidRequestChecksum(frame), "merged frame checksum") &&
        Check(confirm.operation == mdv::DriverOperation::ConfirmRead,
              "merged frame has one confirmation") &&
        Check(driver.DeviceByAddress(3).device.PendingFields() ==
                  mdv::PendingField::None,
              "all merged fields confirmed") &&
        Check(!driver.HasQueuedWork(), "one C3 handles all queued field changes");
}

bool TestSetTimeoutIsConfirmedBeforeRetry()
{
    ScriptedTransport transport({
        SuccessfulTransaction(MakeDriverResponse(
            4, mdv::Command::Read, 0x10, 0x80, 21)),
        mdv::TransactionResult{
            .status = mdv::TransactionStatus::Timeout,
            .response = std::nullopt,
            .error = "set timeout",
        },
        SuccessfulTransaction(MakeDriverResponse(
            4, mdv::Command::Read, 0x10, 0x80, 21)),
        SuccessfulTransaction(MakeDriverResponse(
            4, mdv::Command::Set, 0x10, 0x80, 21)),
        SuccessfulTransaction(MakeDriverResponse(
            4, mdv::Command::Read, 0x90, 0x80, 21)),
    });
    mdv::MdvDriver driver({4}, transport);

    static_cast<void>(driver.ProcessNext());
    driver.SetPower(4, true);

    const auto timeout = driver.ProcessNext();
    const auto confirmOld = driver.ProcessNext();
    const auto retry = driver.ProcessNext();
    const auto confirmed = driver.ProcessNext();
    const auto& runtime = driver.DeviceByAddress(4);

    return Check(timeout.operation == mdv::DriverOperation::SetState &&
                     timeout.outcome == mdv::DriverOutcome::Timeout,
                 "C3 timeout is reported") &&
        Check(confirmOld.operation == mdv::DriverOperation::ConfirmRead,
              "C0 is sent before duplicate C3") &&
        Check(retry.operation == mdv::DriverOperation::SetState,
              "old C0 result permits cached C3 retry") &&
        Check(confirmed.operation == mdv::DriverOperation::ConfirmRead &&
                  confirmed.outcome == mdv::DriverOutcome::Success,
              "retry is confirmed") &&
        Check(runtime.failedSets == 1 && runtime.successfulSets == 1,
              "set counters include timeout and retry") &&
        Check(runtime.device.PendingFields() == mdv::PendingField::None,
              "retry confirmation clears pending Power");
}

bool TestDriverRejectsCommandBeforeFirstRead()
{
    ScriptedTransport transport({});
    mdv::MdvDriver driver({7}, transport);

    bool rejected = false;
    try {
        driver.SetPower(7, true);
    }
    catch (const std::logic_error&) {
        rejected = true;
    }

    return Check(rejected, "driver rejects C3 before cached frame initialization") &&
        Check(!driver.HasQueuedWork(), "rejected command is not queued");
}


bool TestLockUnlockRequests()
{
    constexpr RequestFrame expectedLock{
        0xAA, 0xCC, 0x01, 0x00, 0x80, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x33, 0x80, 0x55};
    constexpr RequestFrame expectedUnlock{
        0xAA, 0xCD, 0x01, 0x00, 0x80, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0x80, 0x55};

    return CheckFrame(mdv::BuildLockRequest(1), expectedLock, "CC lock frame") &&
        CheckFrame(mdv::BuildUnlockRequest(1), expectedUnlock, "CD unlock frame") &&
        Check(mdv::HasValidRequestChecksum(expectedLock), "CC checksum") &&
        Check(mdv::HasValidRequestChecksum(expectedUnlock), "CD checksum");
}

bool TestBlockStateParsing()
{
    const auto response = MakeDriverResponse(
        8, mdv::Command::Read, 0xA8, 0x84, 23);
    const auto parsed = mdv::ParseResponse(response, 8);

    return Check(parsed.ok, parsed.error) &&
        Check(parsed.state.power, "blocked response Power") &&
        Check(parsed.state.mode == mdv::Mode::Cool, "blocked response Mode") &&
        Check(parsed.state.modeLocked, "blocked response Blok") ;
}

bool TestBlockQueueAndConfirmation()
{
    ScriptedTransport transport({
        SuccessfulTransaction(MakeDriverResponse(8)),
        SuccessfulTransaction(MakeDriverResponse(8, mdv::Command::Lock)),
        SuccessfulTransaction(MakeDriverResponse(
            8, mdv::Command::Read, 0xA8, 0x84, 23)),
        SuccessfulTransaction(MakeDriverResponse(
            8, mdv::Command::Unlock, 0xA8, 0x84, 23)),
        SuccessfulTransaction(MakeDriverResponse(8)),
    });
    mdv::MdvDriver driver({8}, transport);

    static_cast<void>(driver.ProcessNext());
    driver.SetBlocked(8, true);
    const auto lock = driver.ProcessNext();
    const auto confirmLock = driver.ProcessNext();
    const bool lockedConfirmed = !driver.DeviceByAddress(8).blockPending &&
        driver.DeviceByAddress(8).device.ActualState().modeLocked;

    driver.SetBlocked(8, false);
    const auto unlock = driver.ProcessNext();
    const auto confirmUnlock = driver.ProcessNext();
    const auto& runtime = driver.DeviceByAddress(8);

    const bool order = transport.requests.size() == 5 &&
        transport.requests[1][1] == static_cast<std::uint8_t>(mdv::Command::Lock) &&
        transport.requests[2][1] == static_cast<std::uint8_t>(mdv::Command::Read) &&
        transport.requests[3][1] == static_cast<std::uint8_t>(mdv::Command::Unlock) &&
        transport.requests[4][1] == static_cast<std::uint8_t>(mdv::Command::Read);

    return Check(lock.operation == mdv::DriverOperation::Lock &&
                     lock.outcome == mdv::DriverOutcome::Success,
                 "Blok=1 sends CC") &&
        Check(confirmLock.operation == mdv::DriverOperation::ConfirmRead,
              "CC is followed by C0") &&
        Check(lockedConfirmed, "C0 confirms Blok=1") &&
        Check(unlock.operation == mdv::DriverOperation::Unlock &&
                  unlock.outcome == mdv::DriverOutcome::Success,
              "Blok=0 sends CD") &&
        Check(confirmUnlock.operation == mdv::DriverOperation::ConfirmRead,
              "CD is followed by C0") &&
        Check(!runtime.blockPending && !runtime.device.ActualState().modeLocked,
              "C0 confirms Blok=0") &&
        Check(runtime.successfulBlockCommands == 2,
              "block command success counter") &&
        Check(order, "CC/CD confirmation order");
}

class FakeMqttClient final : public mdv::IMqttClient {
public:
    void SetMessageHandler(MessageHandler handler) override
    {
        handler_ = std::move(handler);
    }

    void Subscribe(std::string_view topicFilter) override
    {
        subscriptions.emplace_back(topicFilter);
    }

    void Publish(
        std::string_view topic,
        std::string_view payload,
        bool retained) override
    {
        publications.push_back(mdv::MqttPublication{
            .topic = std::string(topic),
            .payload = std::string(payload),
            .retained = retained,
        });
    }

    void Emit(std::string topic, std::string payload, bool retained = false)
    {
        if (!handler_) {
            throw std::logic_error("fake MQTT client has no message handler");
        }
        handler_(mdv::MqttMessage{
            .topic = std::move(topic),
            .payload = std::move(payload),
            .retained = retained,
        });
    }

    std::vector<std::string> subscriptions;
    std::vector<mdv::MqttPublication> publications;

private:
    MessageHandler handler_;
};

bool TestMqttCommandQueueAndRouting()
{
    ScriptedTransport transport({
        SuccessfulTransaction(MakeDriverResponse(
            9, mdv::Command::Read, 0x10, 0x80, 21)),
    });
    mdv::MdvDriver driver({9}, transport);
    static_cast<void>(driver.ProcessNext());

    FakeMqttClient client;
    mdv::MqttCommandRouter router(1, driver);
    mdv::MqttCommandService service(client, router);
    service.Start();

    const auto before = driver.DeviceByAddress(9).device.CachedSetFrame();
    client.Emit("/devices/Fan-1_9/controls/Power/on1", "1");
    client.Emit("/devices/Fan-1_9/controls/Mode/on1", "1");
    client.Emit("/devices/Fan-1_9/controls/Speed/on1", "2");
    client.Emit("/devices/Fan-1_9/controls/SetTemp/on1", "25");
    client.Emit("/devices/Fan-1_9/controls/Blinds/on1", "1");
    client.Emit("/devices/Fan-1_9/controls/Blok/on1", "1");

    const bool callbackDidNotTouchDriver =
        driver.DeviceByAddress(9).device.CachedSetFrame() == before &&
        service.PendingCount() == 6;

    bool allApplied = true;
    for (int index = 0; index < 6; ++index) {
        const auto result = service.ProcessOne();
        allApplied = allApplied && result.has_value() &&
            result->status == mdv::MqttCommandStatus::Applied;
    }

    const auto& runtime = driver.DeviceByAddress(9);
    const auto& frame = runtime.device.CachedSetFrame();

    return Check(client.subscriptions.size() == 1 &&
                     client.subscriptions[0] == "/devices/+/controls/+/on1",
                 "MQTT command subscription") &&
        Check(callbackDidNotTouchDriver,
              "MQTT callback only queues messages") &&
        Check(allApplied && service.PendingCount() == 0,
              "queued MQTT commands applied by driver thread") &&
        Check(frame[6] == 0x84, "MQTT Power and Heat mapping") &&
        Check(frame[7] == 0x02, "MQTT Medium speed mapping") &&
        Check(frame[8] == 25, "MQTT SetTemp mapping") &&
        Check(frame[9] == 0x04, "MQTT Blinds mapping") &&
        Check(runtime.blockPending && runtime.desiredBlocked,
              "MQTT Blok mapping") &&
        Check(driver.HasQueuedWork(), "MQTT commands queue RS-485 work");
}

bool TestMqttStatePublishingOnlyChanges()
{
    ScriptedTransport transport({
        SuccessfulTransaction(MakeDriverResponse(11)),
        SuccessfulTransaction(MakeDriverResponse(11)),
        SuccessfulTransaction(MakeDriverResponse(
            11, mdv::Command::Read, 0x88, 0x84, 23, 0, 0x5A)),
    });
    mdv::MdvDriver driver({11}, transport);
    FakeMqttClient client;
    mdv::MqttStatePublisher publisher(1, client);

    const auto first = driver.ProcessNext();
    publisher.PublishAfter(driver, first);
    const auto initialCount = client.publications.size();

    const auto unchanged = driver.ProcessNext();
    publisher.PublishAfter(driver, unchanged);
    const auto unchangedCount = client.publications.size();

    const auto changed = driver.ProcessNext();
    publisher.PublishAfter(driver, changed);

    const bool initialTopics = initialCount == 10 &&
        client.publications[0].topic == "/devices/Fan-1_11/controls/Power/on" &&
        client.publications[0].payload == "1" &&
        client.publications[1].topic == "/devices/Fan-1_11/controls/Mode/on" &&
        client.publications[1].payload == "0" &&
        client.publications[2].topic == "/devices/Fan-1_11/controls/Speed/on" &&
        client.publications[2].payload == "4" &&
        client.publications[3].topic == "/devices/Fan-1_11/controls/SetTemp/on" &&
        client.publications[3].payload == "23" &&
        client.publications[4].topic == "/devices/Fan-1_11/controls/Temp/on" &&
        client.publications[4].payload == "24" &&
        client.publications[5].topic == "/devices/Fan-1_11/controls/Blinds/on" &&
        client.publications[6].topic == "/devices/Fan-1_11/controls/Blok/on" &&
        client.publications[7].topic == "/devices/Fan-1_11/controls/Alarm/on" &&
        client.publications[8].topic == "/devices/Fan-1_11/controls/AlarmCode/on" &&
        client.publications[9].topic == "/devices/Fan-1_11/controls/Status/on" &&
        client.publications[9].payload == "1";

    const auto& last = client.publications.back();
    const bool allNonRetained = std::all_of(
        client.publications.begin(), client.publications.end(),
        [](const mdv::MqttPublication& publication) {
            return !publication.retained;
        });

    return Check(first.outcome == mdv::DriverOutcome::Success,
                 "first C0 is available for MQTT state") &&
        Check(initialTopics, "first C0 publishes all supported /on values") &&
        Check(unchangedCount == initialCount,
              "unchanged C0 produces no MQTT publications") &&
        Check(client.publications.size() == initialCount + 1 &&
                  last.topic == "/devices/Fan-1_11/controls/Temp/on" &&
                  last.payload == "25",
              "only changed temperature is published") &&
        Check(allNonRetained, "state /on publications are not retained");
}

bool TestMqttAlarmOfflineAndRecovery()
{
    ScriptedTransport transport({
        SuccessfulTransaction(MakeDriverResponse(12)),
        SuccessfulTransaction(MakeDriverResponse(
            12, mdv::Command::Read, 0x88, 0x84, 23, 0, 0x58, 0x04)),
        mdv::TransactionResult{
            .status = mdv::TransactionStatus::Timeout,
            .response = std::nullopt,
            .error = "poll timeout",
        },
        mdv::TransactionResult{
            .status = mdv::TransactionStatus::Timeout,
            .response = std::nullopt,
            .error = "poll timeout",
        },
        SuccessfulTransaction(MakeDriverResponse(12)),
    });
    mdv::MdvDriver driver({12}, transport);
    FakeMqttClient client;
    mdv::MqttStatePublisher publisher(1, client);

    auto result = driver.ProcessNext();
    publisher.PublishAfter(driver, result);
    client.publications.clear();

    result = driver.ProcessNext();
    publisher.PublishAfter(driver, result);
    const auto alarmCount = client.publications.size();

    result = driver.ProcessNext();
    publisher.PublishAfter(driver, result);
    const auto offlineCount = client.publications.size();

    result = driver.ProcessNext();
    publisher.PublishAfter(driver, result);
    const auto repeatedOfflineCount = client.publications.size();

    result = driver.ProcessNext();
    publisher.PublishAfter(driver, result);

    const bool alarmPublications = alarmCount == 3 &&
        client.publications[0].topic.ends_with("/Alarm/on") &&
        client.publications[0].payload == "1" &&
        client.publications[1].topic.ends_with("/AlarmCode/on") &&
        client.publications[1].payload == "3" &&
        client.publications[2].topic.ends_with("/Status/on") &&
        client.publications[2].payload == "6";

    const bool offlinePublications = offlineCount == alarmCount + 2 &&
        client.publications[3].topic.ends_with("/Alarm/on") &&
        client.publications[3].payload == "2" &&
        client.publications[4].topic.ends_with("/Status/on") &&
        client.publications[4].payload == "7";

    const bool recoveryPublications = client.publications.size() == offlineCount + 3 &&
        client.publications[5].topic.ends_with("/Alarm/on") &&
        client.publications[5].payload == "0" &&
        client.publications[6].topic.ends_with("/AlarmCode/on") &&
        client.publications[6].payload == "0" &&
        client.publications[7].topic.ends_with("/Status/on") &&
        client.publications[7].payload == "1";

    return Check(alarmPublications, "E2 publishes Alarm=1, AlarmCode=3 and Status=6") &&
        Check(offlinePublications, "timeout publishes Alarm=2 and Status=7") &&
        Check(repeatedOfflineCount == offlineCount,
              "repeated timeout does not repeat offline state") &&
        Check(recoveryPublications,
              "recovery publishes cleared alarm and working status");
}

bool TestMqttBlockDoesNotReplaceStatus()
{
    ScriptedTransport transport({
        SuccessfulTransaction(MakeDriverResponse(
            13, mdv::Command::Read, 0xA8, 0x84, 23)),
    });
    mdv::MdvDriver driver({13}, transport);
    FakeMqttClient client;
    mdv::MqttStatePublisher publisher(1, client);

    const auto result = driver.ProcessNext();
    publisher.PublishAfter(driver, result);

    const auto blocked = std::find_if(
        client.publications.begin(), client.publications.end(),
        [](const mdv::MqttPublication& publication) {
            return publication.topic.ends_with("/Blok/on");
        });
    const auto status = std::find_if(
        client.publications.begin(), client.publications.end(),
        [](const mdv::MqttPublication& publication) {
            return publication.topic.ends_with("/Status/on");
        });

    return Check(blocked != client.publications.end() && blocked->payload == "1",
                 "Blok is published separately") &&
        Check(status != client.publications.end() && status->payload == "1",
              "Blok does not replace Cool status");
}

bool TestMosquittoClientBuffering()
{
    mdv::MqttConnectionOptions options;
    options.host = "127.0.0.1";
    options.port = 1883;
    options.keepAliveSeconds = 60;
    options.clientId = "mdvwb-self-test";
    mdv::MosquittoMqttClient client(options);

    client.Subscribe(mdv::MqttCommandRouter::SubscriptionTopic());
    client.Subscribe(mdv::MqttCommandRouter::SubscriptionTopic());
    client.Publish("/devices/Fan-1_1/controls/Power/on", "0", false);
    client.Publish("/devices/Fan-1_1/controls/Power/on", "1", false);
    client.Publish("/devices/Fan-1_1/controls/Mode/on", "4", false);

    bool invalidHostRejected = false;
    bool invalidPortRejected = false;
    try {
        auto invalidOptions = mdv::MqttConnectionOptions{};
        invalidOptions.host.clear();
        mdv::MosquittoMqttClient invalid(invalidOptions);
    }
    catch (const std::invalid_argument&) {
        invalidHostRejected = true;
    }
    try {
        auto invalidOptions = mdv::MqttConnectionOptions{};
        invalidOptions.port = 70000;
        mdv::MosquittoMqttClient invalid(invalidOptions);
    }
    catch (const std::invalid_argument&) {
        invalidPortRejected = true;
    }

    return Check(!client.IsConnected(), "MQTT client starts disconnected") &&
        Check(client.SubscriptionCount() == 1,
              "duplicate MQTT subscription is stored once") &&
        Check(client.PendingPublicationCount() == 2,
              "offline MQTT buffer keeps latest value per topic") &&
        Check(invalidHostRejected, "empty MQTT host rejected") &&
        Check(invalidPortRejected, "invalid MQTT port rejected");
}

bool TestMqttValidation()
{
    ScriptedTransport transport({});
    mdv::MdvDriver driver({10}, transport);
    mdv::MqttCommandRouter router(1, driver);

    const auto wrongBus = router.Handle({
        "/devices/Fan-2_10/controls/Power/on1", "1", false});
    const auto loopTopic = router.Handle({
        "/devices/Fan-1_10/controls/Power/on", "1", false});
    const auto badPayload = router.Handle({
        "/devices/Fan-1_10/controls/Power/on1", "2", false});
    const auto retained = router.Handle({
        "/devices/Fan-1_10/controls/Power/on1", "1", true});
    const auto notInitialized = router.Handle({
        "/devices/Fan-1_10/controls/Power/on1", "1", false});
    const auto unknownDevice = router.Handle({
        "/devices/Fan-1_11/controls/Power/on1", "1", false});

    return Check(wrongBus.status == mdv::MqttCommandStatus::Ignored,
                 "other bus MQTT command ignored") &&
        Check(loopTopic.status == mdv::MqttCommandStatus::InvalidTopic,
              "state /on topic is never treated as command") &&
        Check(badPayload.status == mdv::MqttCommandStatus::InvalidPayload,
              "invalid boolean MQTT payload rejected") &&
        Check(retained.status == mdv::MqttCommandStatus::InvalidPayload,
              "retained MQTT command rejected") &&
        Check(notInitialized.status == mdv::MqttCommandStatus::DeviceNotInitialized,
              "MQTT command waits for first valid C0") &&
        Check(unknownDevice.status == mdv::MqttCommandStatus::DeviceNotConfigured,
              "unconfigured MQTT device rejected");
}

} // namespace

int RunProtocolSelfTest()
{
    const bool ok = TestReadRequests() &&
        TestLockUnlockRequests() &&
        TestSetRequests() &&
        TestResponseParsing() &&
        TestAutoModeParsing() &&
        TestBlockStateParsing() &&
        TestStreamCollector() &&
        TestCachedSetFrame() &&
        TestSingleFieldModification() &&
        TestOldReadDoesNotOverwriteCommand() &&
        TestLocalPanelSynchronizesFreeFields() &&
        TestSafeFallbackAndRevision() &&
        TestUnsafeUseIsRejected() &&
        TestWireRequest() &&
        TestTransactionTiming() &&
        TestInvalidTimingRejected() &&
        TestPortNameNormalization() &&
        TestRoundRobinPolling() &&
        TestPollingContinuesAfterTimeout() &&
        TestInvalidPollingResponses() &&
        TestPollingAddressValidation() &&
        TestSetQueueAndConfirmation() &&
        TestMultipleCommandsMergeIntoOneFrame() &&
        TestSetTimeoutIsConfirmedBeforeRetry() &&
        TestDriverRejectsCommandBeforeFirstRead() &&
        TestBlockQueueAndConfirmation() &&
        TestMqttCommandQueueAndRouting() &&
        TestMqttStatePublishingOnlyChanges() &&
        TestMqttAlarmOfflineAndRecovery() &&
        TestMqttBlockDoesNotReplaceStatus() &&
        TestMosquittoClientBuffering() &&
        TestMqttValidation();

    if (!ok) {
        return 1;
    }

    std::cout << "MDV protocol, cache, serial, polling, command and real MQTT client self-test: OK\n";
    return 0;
}

int main(int argc, char* argv[])
{
    if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
        return RunProtocolSelfTest();
    }

    std::cout << "MDVWB step 7: changed-only MQTT state publishing is ready.\n"
                 "Run with --self-test to verify known MDV behavior.\n";
    return 0;
}
