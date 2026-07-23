#include "mdv_device.h"

#include <stdexcept>

namespace mdv {
namespace {

constexpr std::uint8_t kPowerBit = 0x80;
constexpr std::uint8_t kModeMask = 0x1F;
constexpr std::uint8_t kBlindsBit = 0x04;
constexpr std::uint8_t kKnownFunctionsMask = 0x0F;

[[nodiscard]] bool HasFlag(PendingField value, PendingField flag) noexcept
{
    return (value & flag) != PendingField::None;
}

[[nodiscard]] bool IsValidSetTemperature(std::uint8_t value) noexcept
{
    return value >= 16 && value <= 32;
}

[[nodiscard]] bool FramePower(const RequestFrame& frame) noexcept
{
    return (frame[6] & kPowerBit) != 0;
}

[[nodiscard]] Mode FrameMode(const RequestFrame& frame) noexcept
{
    return static_cast<Mode>(frame[6] & kModeMask);
}

[[nodiscard]] FanSpeed FrameFanSpeed(const RequestFrame& frame) noexcept
{
    return static_cast<FanSpeed>(frame[7]);
}

[[nodiscard]] bool FrameBlinds(const RequestFrame& frame) noexcept
{
    return (frame[9] & kBlindsBit) != 0;
}

} // namespace

DeviceContext::DeviceContext(std::uint8_t address, std::uint8_t masterId)
    : address_(address), masterId_(masterId)
{
    // Reuse protocol validation without creating a second address validator.
    static_cast<void>(BuildReadRequest(address_, masterId_));
}

std::uint8_t DeviceContext::Address() const noexcept
{
    return address_;
}

bool DeviceContext::IsInitialized() const noexcept
{
    return initialized_;
}

bool DeviceContext::HasActualState() const noexcept
{
    return hasActualState_;
}

const DeviceState& DeviceContext::ActualState() const
{
    if (!hasActualState_) {
        throw std::logic_error("MDV actual state is not available yet");
    }
    return actualState_;
}

const RequestFrame& DeviceContext::CachedSetFrame() const
{
    RequireInitialized();
    return setFrame_;
}

PendingField DeviceContext::PendingFields() const noexcept
{
    return pendingFields_;
}

bool DeviceContext::HasPendingField(PendingField field) const noexcept
{
    return HasFlag(pendingFields_, field);
}

bool DeviceContext::IsSetCommandQueued() const noexcept
{
    return setCommandQueued_;
}

std::uint64_t DeviceContext::DesiredRevision() const noexcept
{
    return desiredRevision_;
}

void DeviceContext::SynchronizeReadState(const DeviceState& state)
{
    if (state.command != Command::Read) {
        throw std::invalid_argument("only a C0 response may synchronize MDV state");
    }
    if (state.address != address_ || state.masterId != masterId_) {
        throw std::invalid_argument("MDV response does not belong to this device");
    }

    actualState_ = state;
    hasActualState_ = true;

    if (!initialized_) {
        InitializeSetFrame(state);
        return;
    }

    if (HasPendingField(PendingField::Power)) {
        if (state.power == FramePower(setFrame_)) {
            ClearPending(PendingField::Power);
        }
    }
    else {
        SetRequestPower(setFrame_, state.power);
    }

    if (HasPendingField(PendingField::Mode)) {
        if (state.mode.has_value() && *state.mode == FrameMode(setFrame_)) {
            ClearPending(PendingField::Mode);
        }
    }
    else if (state.mode.has_value()) {
        SetRequestMode(setFrame_, *state.mode);
    }

    if (HasPendingField(PendingField::Speed)) {
        if (state.fanSpeed.has_value() && *state.fanSpeed == FrameFanSpeed(setFrame_)) {
            ClearPending(PendingField::Speed);
        }
    }
    else if (state.fanSpeed.has_value()) {
        SetRequestFanSpeed(setFrame_, *state.fanSpeed);
    }

    if (HasPendingField(PendingField::SetTemperature)) {
        if (state.setTemperature == setFrame_[8]) {
            ClearPending(PendingField::SetTemperature);
        }
    }
    else if (IsValidSetTemperature(state.setTemperature)) {
        SetRequestTemperature(setFrame_, state.setTemperature);
    }

    if (HasPendingField(PendingField::Blinds)) {
        const auto desiredBlinds = static_cast<std::uint8_t>(setFrame_[9] & kBlindsBit);
        const auto actualOtherFunctions = static_cast<std::uint8_t>(
            state.additionalFunctions & (kKnownFunctionsMask & ~kBlindsBit));
        setFrame_[9] = static_cast<std::uint8_t>(
            actualOtherFunctions | desiredBlinds);
        RefreshRequestChecksum(setFrame_);

        if (state.blinds == FrameBlinds(setFrame_)) {
            ClearPending(PendingField::Blinds);
        }
    }
    else {
        setFrame_[9] = static_cast<std::uint8_t>(
            state.additionalFunctions & kKnownFunctionsMask);
        RefreshRequestChecksum(setFrame_);
    }
}

void DeviceContext::ApplyPower(bool power)
{
    RequireInitialized();
    SetRequestPower(setFrame_, power);
    MarkChanged(PendingField::Power);
}

void DeviceContext::ApplyMode(Mode mode)
{
    RequireInitialized();
    SetRequestMode(setFrame_, mode);
    MarkChanged(PendingField::Mode);
}

void DeviceContext::ApplyFanSpeed(FanSpeed speed)
{
    RequireInitialized();
    SetRequestFanSpeed(setFrame_, speed);
    MarkChanged(PendingField::Speed);
}

void DeviceContext::ApplySetTemperature(std::uint8_t temperature)
{
    RequireInitialized();
    SetRequestTemperature(setFrame_, temperature);
    MarkChanged(PendingField::SetTemperature);
}

void DeviceContext::ApplyBlinds(bool enabled)
{
    RequireInitialized();
    SetRequestBlinds(setFrame_, enabled);
    MarkChanged(PendingField::Blinds);
}

SetFrameSnapshot DeviceContext::PrepareSetFrameForSend()
{
    RequireInitialized();
    setCommandQueued_ = false;
    return SetFrameSnapshot{setFrame_, desiredRevision_};
}

void DeviceContext::FinishSetFrameSend(std::uint64_t sentRevision) noexcept
{
    if (desiredRevision_ != sentRevision) {
        setCommandQueued_ = true;
    }
}

void DeviceContext::RequireInitialized() const
{
    if (!initialized_) {
        throw std::logic_error(
            "MDV set command is forbidden before the first valid C0 response");
    }
}

void DeviceContext::MarkChanged(PendingField field) noexcept
{
    pendingFields_ = pendingFields_ | field;
    ++desiredRevision_;
    setCommandQueued_ = true;
}

void DeviceContext::ClearPending(PendingField field) noexcept
{
    pendingFields_ = pendingFields_ & ~field;
}

void DeviceContext::InitializeSetFrame(const DeviceState& state)
{
    SetState initial;
    initial.power = state.power;

    // Some powered-off fan coils answer with zero mode/speed. Auto is the safe
    // fallback because an outgoing C3 frame must still contain one valid value.
    initial.mode = state.mode.value_or(Mode::Auto);
    initial.fanSpeed = state.fanSpeed.value_or(FanSpeed::Auto);
    initial.setTemperature = IsValidSetTemperature(state.setTemperature)
        ? state.setTemperature
        : 21;
    initial.additionalFunctions = static_cast<std::uint8_t>(
        state.additionalFunctions & kKnownFunctionsMask);

    setFrame_ = BuildSetRequest(address_, initial, masterId_);
    pendingFields_ = PendingField::None;
    initialized_ = true;
    setCommandQueued_ = false;
}

} // namespace mdv
