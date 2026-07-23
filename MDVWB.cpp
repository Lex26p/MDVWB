#include "MDVWB.h"
#include "mdv_device.h"
#include "mdv_protocol.h"
#include "mdv_serial.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
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

} // namespace

int RunProtocolSelfTest()
{
    const bool ok = TestReadRequests() &&
        TestSetRequests() &&
        TestResponseParsing() &&
        TestAutoModeParsing() &&
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
        TestPortNameNormalization();

    if (!ok) {
        return 1;
    }

    std::cout << "MDV protocol, device-cache and serial self-test: OK\n";
    return 0;
}

int main(int argc, char* argv[])
{
    if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
        return RunProtocolSelfTest();
    }

    std::cout << "MDVWB step 3: serial transport and 150 ms pacing are ready.\n"
                 "Run with --self-test to verify known MDV behavior.\n";
    return 0;
}
