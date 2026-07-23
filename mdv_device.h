#pragma once

#include "mdv_protocol.h"

#include <cstdint>
#include <optional>
#include <stdexcept>

namespace mdv {

enum class PendingField : std::uint8_t {
    None = 0,
    Power = 1U << 0,
    Mode = 1U << 1,
    Speed = 1U << 2,
    SetTemperature = 1U << 3,
    Blinds = 1U << 4,
};

constexpr PendingField operator|(PendingField left, PendingField right) noexcept
{
    return static_cast<PendingField>(
        static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

constexpr PendingField operator&(PendingField left, PendingField right) noexcept
{
    return static_cast<PendingField>(
        static_cast<std::uint8_t>(left) & static_cast<std::uint8_t>(right));
}

constexpr PendingField operator~(PendingField value) noexcept
{
    return static_cast<PendingField>(~static_cast<std::uint8_t>(value));
}

struct SetFrameSnapshot {
    RequestFrame frame{};
    std::uint64_t revision = 0;
};

// Holds the confirmed state and the complete cached C3 frame for one device.
// The class is intentionally not thread-safe; the future driver will guard it
// with one mutex around MQTT callbacks and the RS-485 worker.
class DeviceContext {
public:
    explicit DeviceContext(std::uint8_t address, std::uint8_t masterId = 0);

    [[nodiscard]] std::uint8_t Address() const noexcept;
    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] bool HasActualState() const noexcept;
    [[nodiscard]] const DeviceState& ActualState() const;
    [[nodiscard]] const RequestFrame& CachedSetFrame() const;
    [[nodiscard]] PendingField PendingFields() const noexcept;
    [[nodiscard]] bool HasPendingField(PendingField field) const noexcept;
    [[nodiscard]] bool IsSetCommandQueued() const noexcept;
    [[nodiscard]] std::uint64_t DesiredRevision() const noexcept;

    // Only a verified C0 response may synchronize the cache. A C3 response can
    // still contain old values and must not call this method.
    void SynchronizeReadState(const DeviceState& state);

    void ApplyPower(bool power);
    void ApplyMode(Mode mode);
    void ApplyFanSpeed(FanSpeed speed);
    void ApplySetTemperature(std::uint8_t temperature);
    void ApplyBlinds(bool enabled);

    // The queue owner takes an immutable frame copy. If MQTT changes the cache
    // while that copy is being sent, FinishSetFrameSend requeues the device.
    [[nodiscard]] SetFrameSnapshot PrepareSetFrameForSend();
    void FinishSetFrameSend(std::uint64_t sentRevision) noexcept;

private:
    void RequireInitialized() const;
    void MarkChanged(PendingField field) noexcept;
    void ClearPending(PendingField field) noexcept;
    void InitializeSetFrame(const DeviceState& state);

    std::uint8_t address_ = 0;
    std::uint8_t masterId_ = 0;
    DeviceState actualState_{};
    RequestFrame setFrame_{};
    PendingField pendingFields_ = PendingField::None;
    std::uint64_t desiredRevision_ = 0;
    bool hasActualState_ = false;
    bool initialized_ = false;
    bool setCommandQueued_ = false;
};

} // namespace mdv
