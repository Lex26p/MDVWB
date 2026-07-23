#include "mdv_protocol.h"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace mdv {
namespace {

constexpr std::uint8_t kPowerBit = 0x80;
constexpr std::uint8_t kModeMask = 0x1F;
constexpr std::uint8_t kPhysicalSpeedMask = 0x07;
constexpr std::uint8_t kAutoSpeedBit = 0x80;
constexpr std::uint8_t kKnownFunctionsMask = 0x0F;
constexpr std::uint8_t kBlindsBit = 0x04;

void ValidateSetFrame(const RequestFrame& frame)
{
    if (frame[0] != kFrameStart || frame[15] != kFrameEnd ||
        frame[1] != static_cast<std::uint8_t>(Command::Set)) {
        throw std::invalid_argument("frame is not an MDV C3 set request");
    }
}

void ValidateMode(Mode mode)
{
    const auto value = static_cast<std::uint8_t>(mode);
    if (std::popcount(static_cast<unsigned int>(value & kModeMask)) != 1 ||
        (value & ~kModeMask) != 0) {
        throw std::invalid_argument("MDV set frame must contain exactly one mode");
    }
}

void ValidateFanSpeed(FanSpeed speed)
{
    const auto value = static_cast<std::uint8_t>(speed);
    const bool valid = value == static_cast<std::uint8_t>(FanSpeed::High) ||
        value == static_cast<std::uint8_t>(FanSpeed::Medium) ||
        value == static_cast<std::uint8_t>(FanSpeed::Low) ||
        value == static_cast<std::uint8_t>(FanSpeed::Auto);
    if (!valid) {
        throw std::invalid_argument("MDV set frame must contain exactly one fan speed");
    }
}

void ValidateTemperature(std::uint8_t temperature)
{
    if (temperature < 16 || temperature > 32) {
        throw std::invalid_argument("MDV set temperature must be in range 16..32");
    }
}

[[nodiscard]] bool IsValidAddress(std::uint8_t address) noexcept
{
    return address <= kMaxDeviceAddress;
}

[[nodiscard]] bool IsValidMasterId(std::uint8_t masterId) noexcept
{
    return masterId <= kMaxDeviceAddress;
}

[[nodiscard]] std::uint8_t EncodeMode(const SetState& state) noexcept
{
    return static_cast<std::uint8_t>(
        (state.power ? kPowerBit : 0U) |
        static_cast<std::uint8_t>(state.mode));
}

[[nodiscard]] std::optional<Mode> DecodeSingleMode(std::uint8_t bits) noexcept
{
    switch (bits) {
        case static_cast<std::uint8_t>(Mode::Fan):
            return Mode::Fan;
        case static_cast<std::uint8_t>(Mode::Dry):
            return Mode::Dry;
        case static_cast<std::uint8_t>(Mode::Heat):
            return Mode::Heat;
        case static_cast<std::uint8_t>(Mode::Cool):
            return Mode::Cool;
        case static_cast<std::uint8_t>(Mode::Auto):
            return Mode::Auto;
        default:
            return std::nullopt;
    }
}

[[nodiscard]] std::optional<FanSpeed> DecodePhysicalSpeed(std::uint8_t bits) noexcept
{
    switch (bits) {
        case static_cast<std::uint8_t>(FanSpeed::High):
            return FanSpeed::High;
        case static_cast<std::uint8_t>(FanSpeed::Medium):
            return FanSpeed::Medium;
        case static_cast<std::uint8_t>(FanSpeed::Low):
            return FanSpeed::Low;
        default:
            return std::nullopt;
    }
}

[[nodiscard]] bool DecodeModeByte(
    std::uint8_t raw,
    DeviceState& state,
    std::string& error)
{
    state.power = (raw & kPowerBit) != 0;
    state.modeLocked = (raw & 0x20U) != 0;

    const auto modeBits = static_cast<std::uint8_t>(raw & kModeMask);
    const bool isAuto = (modeBits & static_cast<std::uint8_t>(Mode::Auto)) != 0;
    const auto activeBits = static_cast<std::uint8_t>(
        modeBits & ~static_cast<std::uint8_t>(Mode::Auto));

    if (std::popcount(activeBits) > 1) {
        error = "response contains several active MDV modes";
        return false;
    }

    if (isAuto) {
        state.mode = Mode::Auto;
        state.activeMode = DecodeSingleMode(activeBits);
        return true;
    }

    if (activeBits == 0) {
        state.mode.reset();
        state.activeMode.reset();
        return true;
    }

    state.mode = DecodeSingleMode(activeBits);
    state.activeMode = state.mode;
    if (!state.mode.has_value()) {
        error = "response contains an unknown MDV mode";
        return false;
    }

    return true;
}

[[nodiscard]] bool DecodeSpeedByte(
    std::uint8_t raw,
    DeviceState& state,
    std::string& error)
{
    const bool isAuto = (raw & kAutoSpeedBit) != 0;
    const auto physicalBits = static_cast<std::uint8_t>(raw & kPhysicalSpeedMask);

    if (std::popcount(physicalBits) > 1) {
        error = "response contains several physical fan speeds";
        return false;
    }

    if (isAuto) {
        state.fanSpeed = FanSpeed::Auto;
        state.activeFanSpeed = DecodePhysicalSpeed(physicalBits);
        return true;
    }

    if (physicalBits == 0) {
        state.fanSpeed.reset();
        state.activeFanSpeed.reset();
        return true;
    }

    state.fanSpeed = DecodePhysicalSpeed(physicalBits);
    state.activeFanSpeed = state.fanSpeed;
    if (!state.fanSpeed.has_value()) {
        error = "response contains an unknown fan speed";
        return false;
    }

    return true;
}

void ValidateSetState(const SetState& state)
{
    ValidateMode(state.mode);
    ValidateFanSpeed(state.fanSpeed);
    ValidateTemperature(state.setTemperature);

    if ((state.additionalFunctions & ~kKnownFunctionsMask) != 0) {
        throw std::invalid_argument("MDV set frame contains reserved function bits");
    }
}

RequestFrame BuildRequestBase(
    Command command,
    std::uint8_t address,
    std::uint8_t masterId)
{
    if (!IsValidAddress(address)) {
        throw std::invalid_argument("MDV device address must be in range 0x00..0x3F");
    }
    if (!IsValidMasterId(masterId)) {
        throw std::invalid_argument("MDV master ID must be in range 0x00..0x3F");
    }

    RequestFrame frame{};
    frame[0] = kFrameStart;
    frame[1] = static_cast<std::uint8_t>(command);
    frame[2] = address;
    frame[3] = masterId;
    frame[4] = 0x80;
    frame[5] = masterId;
    frame[13] = static_cast<std::uint8_t>(~frame[1]);
    frame[15] = kFrameEnd;
    return frame;
}

} // namespace

bool IsKnownCommand(std::uint8_t value) noexcept
{
    return value == static_cast<std::uint8_t>(Command::Read) ||
        value == static_cast<std::uint8_t>(Command::Set) ||
        value == static_cast<std::uint8_t>(Command::Lock) ||
        value == static_cast<std::uint8_t>(Command::Unlock);
}

std::uint8_t CalculateRequestChecksum(const RequestFrame& frame) noexcept
{
    std::uint8_t sum = 0;
    for (std::size_t index = 1; index <= 13; ++index) {
        sum = static_cast<std::uint8_t>(sum + frame[index]);
    }
    return static_cast<std::uint8_t>(0U - sum);
}

bool HasValidRequestChecksum(const RequestFrame& frame) noexcept
{
    std::uint8_t sum = 0;
    for (std::size_t index = 1; index <= 14; ++index) {
        sum = static_cast<std::uint8_t>(sum + frame[index]);
    }
    return sum == 0;
}

void RefreshRequestChecksum(RequestFrame& frame) noexcept
{
    frame[14] = CalculateRequestChecksum(frame);
}

bool HasValidResponseChecksum(const ResponseFrame& frame) noexcept
{
    std::uint8_t sum = 0;
    for (std::size_t index = 1; index <= 30; ++index) {
        sum = static_cast<std::uint8_t>(sum + frame[index]);
    }
    return sum == 0;
}

RequestFrame BuildReadRequest(std::uint8_t address, std::uint8_t masterId)
{
    auto frame = BuildRequestBase(Command::Read, address, masterId);
    RefreshRequestChecksum(frame);
    return frame;
}

RequestFrame BuildLockRequest(std::uint8_t address, std::uint8_t masterId)
{
    auto frame = BuildRequestBase(Command::Lock, address, masterId);
    RefreshRequestChecksum(frame);
    return frame;
}

RequestFrame BuildUnlockRequest(std::uint8_t address, std::uint8_t masterId)
{
    auto frame = BuildRequestBase(Command::Unlock, address, masterId);
    RefreshRequestChecksum(frame);
    return frame;
}

RequestFrame BuildSetRequest(
    std::uint8_t address,
    const SetState& state,
    std::uint8_t masterId)
{
    ValidateSetState(state);

    auto frame = BuildRequestBase(Command::Set, address, masterId);
    frame[6] = EncodeMode(state);
    frame[7] = static_cast<std::uint8_t>(state.fanSpeed);
    frame[8] = state.setTemperature;
    frame[9] = static_cast<std::uint8_t>(state.additionalFunctions & kKnownFunctionsMask);
    RefreshRequestChecksum(frame);
    return frame;
}

void SetRequestPower(RequestFrame& frame, bool power)
{
    ValidateSetFrame(frame);
    frame[6] = power
        ? static_cast<std::uint8_t>(frame[6] | kPowerBit)
        : static_cast<std::uint8_t>(frame[6] & ~kPowerBit);
    RefreshRequestChecksum(frame);
}

void SetRequestMode(RequestFrame& frame, Mode mode)
{
    ValidateSetFrame(frame);
    ValidateMode(mode);
    frame[6] = static_cast<std::uint8_t>(
        (frame[6] & kPowerBit) | static_cast<std::uint8_t>(mode));
    RefreshRequestChecksum(frame);
}

void SetRequestFanSpeed(RequestFrame& frame, FanSpeed speed)
{
    ValidateSetFrame(frame);
    ValidateFanSpeed(speed);
    frame[7] = static_cast<std::uint8_t>(speed);
    RefreshRequestChecksum(frame);
}

void SetRequestTemperature(RequestFrame& frame, std::uint8_t temperature)
{
    ValidateSetFrame(frame);
    ValidateTemperature(temperature);
    frame[8] = temperature;
    RefreshRequestChecksum(frame);
}

void SetRequestBlinds(RequestFrame& frame, bool enabled)
{
    ValidateSetFrame(frame);
    frame[9] = enabled
        ? static_cast<std::uint8_t>(frame[9] | kBlindsBit)
        : static_cast<std::uint8_t>(frame[9] & ~kBlindsBit);
    frame[9] = static_cast<std::uint8_t>(frame[9] & kKnownFunctionsMask);
    RefreshRequestChecksum(frame);
}

ParseResult ParseResponse(
    const ResponseFrame& frame,
    std::optional<std::uint8_t> expectedAddress,
    std::uint8_t expectedMasterId)
{
    ParseResult result;

    if (frame[0] != kFrameStart) {
        result.error = "response does not start with 0xAA";
        return result;
    }
    if (frame[31] != kFrameEnd) {
        result.error = "response does not end with 0x55";
        return result;
    }
    if (!IsKnownCommand(frame[1])) {
        result.error = "response contains an unknown command";
        return result;
    }
    if (frame[2] != 0x80) {
        result.error = "response byte 2 must be 0x80";
        return result;
    }
    if (!IsValidMasterId(expectedMasterId)) {
        result.error = "expected master ID is out of range";
        return result;
    }
    if (frame[3] != expectedMasterId || frame[5] != expectedMasterId) {
        result.error = "response master ID does not match";
        return result;
    }
    if (!IsValidAddress(frame[4])) {
        result.error = "response device address is out of range";
        return result;
    }
    if (expectedAddress.has_value() && frame[4] != *expectedAddress) {
        result.error = "response device address does not match request";
        return result;
    }
    if (!HasValidResponseChecksum(frame)) {
        result.error = "response checksum is invalid";
        return result;
    }

    result.state.command = static_cast<Command>(frame[1]);
    result.state.address = frame[4];
    result.state.masterId = frame[3];

    if (!DecodeModeByte(frame[8], result.state, result.error)) {
        return result;
    }
    if (!DecodeSpeedByte(frame[9], result.state, result.error)) {
        return result;
    }

    result.state.setTemperature = frame[10];
    if (frame[11] != 0xFF) {
        result.state.roomTemperature = static_cast<double>(frame[11]) / 2.0 - 20.0;
    }

    result.state.additionalFunctions = static_cast<std::uint8_t>(frame[20] & kKnownFunctionsMask);
    result.state.blinds = (result.state.additionalFunctions & kBlindsBit) != 0;
    result.state.statusBits = frame[21];
    result.state.errorsE0E7 = frame[22];
    result.state.errorsE8EF = frame[23];
    result.state.protectionsP0P7 = frame[24];
    result.state.protectionsP8PF = frame[25];
    result.state.communicationErrors = frame[26];

    result.ok = true;
    return result;
}

std::optional<ResponseFrame> ResponseFrameCollector::Push(std::uint8_t byte) noexcept
{
    if (size_ == 0) {
        if (byte != kFrameStart) {
            return std::nullopt;
        }
        buffer_[size_++] = byte;
        return std::nullopt;
    }

    buffer_[size_++] = byte;
    if (size_ < kResponseSize) {
        return std::nullopt;
    }

    if (buffer_[kResponseSize - 1] == kFrameEnd) {
        const auto frame = buffer_;
        Reset();
        return frame;
    }

    Resynchronize();
    return std::nullopt;
}

void ResponseFrameCollector::Reset() noexcept
{
    buffer_.fill(0);
    size_ = 0;
}

void ResponseFrameCollector::Resynchronize() noexcept
{
    const auto nextStart = std::find(
        buffer_.begin() + 1,
        buffer_.end(),
        kFrameStart);

    if (nextStart == buffer_.end()) {
        Reset();
        return;
    }

    const auto retainedSize = static_cast<std::size_t>(buffer_.end() - nextStart);
    std::move(nextStart, buffer_.end(), buffer_.begin());
    std::fill(buffer_.begin() + retainedSize, buffer_.end(), 0);
    size_ = retainedSize;
}

} // namespace mdv
