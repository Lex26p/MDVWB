#include "MDVWB.h"
#include "mdv_protocol.h"

#include <array>
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

bool TestInvalidDataIsRejected()
{
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

    return Check(badTemperatureRejected, "unsafe SetTemp is rejected") &&
        Check(!mdv::ParseResponse(corrupted, 0).ok, "invalid response checksum is rejected");
}

} // namespace

int RunProtocolSelfTest()
{
    const bool ok = TestReadRequests() &&
        TestSetRequests() &&
        TestResponseParsing() &&
        TestAutoModeParsing() &&
        TestStreamCollector() &&
        TestInvalidDataIsRejected();

    if (!ok) {
        return 1;
    }

    std::cout << "MDV protocol self-test: OK\n";
    return 0;
}

int main(int argc, char* argv[])
{
    if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
        return RunProtocolSelfTest();
    }

    std::cout << "MDVWB step 1: protocol module is ready.\n"
                 "Run with --self-test to verify known MDV frames.\n";
    return 0;
}
