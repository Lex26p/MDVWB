#include "mdv_serial.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace mdv {
namespace {

void ValidateTiming(const TimingSettings& settings)
{
    if (settings.transactionPeriod.count() <= 0) {
        throw std::invalid_argument("MDV transaction period must be positive");
    }
    if (settings.responseTimeout.count() <= 0) {
        throw std::invalid_argument("MDV response timeout must be positive");
    }
    if (settings.responseTimeout >= settings.transactionPeriod) {
        throw std::invalid_argument(
            "MDV response timeout must be shorter than transaction period");
    }
}

#ifdef _WIN32

[[nodiscard]] HANDLE ToHandle(void* handle) noexcept
{
    return static_cast<HANDLE>(handle);
}

[[nodiscard]] std::runtime_error WindowsError(std::string_view operation)
{
    const auto code = GetLastError();
    return std::runtime_error(
        std::string(operation) + " failed, Windows error " + std::to_string(code));
}

#else

[[nodiscard]] std::runtime_error PosixError(std::string_view operation)
{
    return std::runtime_error(
        std::string(operation) + " failed: " + std::strerror(errno));
}

#endif

} // namespace

WireRequest BuildWireRequest(const RequestFrame& frame) noexcept
{
    WireRequest wire{};
    wire[0] = kTransportPadding;
    std::copy(frame.begin(), frame.end(), wire.begin() + 1);
    return wire;
}

TransactionPacer::TransactionPacer(TimingSettings settings)
    : settings_(settings)
{
    ValidateTiming(settings_);
}

std::chrono::steady_clock::time_point TransactionPacer::WaitForNextStart()
{
    if (nextAllowedStart_.has_value()) {
        std::this_thread::sleep_until(*nextAllowedStart_);
    }

    const auto actualStart = std::chrono::steady_clock::now();
    nextAllowedStart_ = NextAllowedStart(actualStart);
    return actualStart;
}

std::chrono::steady_clock::time_point TransactionPacer::ResponseDeadline(
    std::chrono::steady_clock::time_point transactionStart) const noexcept
{
    return transactionStart + settings_.responseTimeout;
}

std::chrono::steady_clock::time_point TransactionPacer::NextAllowedStart(
    std::chrono::steady_clock::time_point transactionStart) const noexcept
{
    return transactionStart + settings_.transactionPeriod;
}

const TimingSettings& TransactionPacer::Settings() const noexcept
{
    return settings_;
}

void TransactionPacer::Reset() noexcept
{
    nextAllowedStart_.reset();
}

SerialPort::~SerialPort()
{
    Close();
}

SerialPort::SerialPort(SerialPort&& other) noexcept
    : handle_(other.handle_), portName_(std::move(other.portName_))
{
#ifdef _WIN32
    other.handle_ = reinterpret_cast<void*>(-1);
#else
    other.handle_ = -1;
#endif
}

SerialPort& SerialPort::operator=(SerialPort&& other) noexcept
{
    if (this != &other) {
        Close();
        handle_ = other.handle_;
        portName_ = std::move(other.portName_);
#ifdef _WIN32
        other.handle_ = reinterpret_cast<void*>(-1);
#else
        other.handle_ = -1;
#endif
    }
    return *this;
}

void SerialPort::Open(std::string_view portName)
{
    Close();
    portName_ = NormalizePortName(portName);

#ifdef _WIN32
    const auto handle = CreateFileA(
        portName_.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        portName_.clear();
        throw WindowsError("opening serial port");
    }
    handle_ = handle;

    try {
        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(handle, &dcb)) {
            throw WindowsError("reading serial settings");
        }

        dcb.BaudRate = CBR_4800;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fDsrSensitivity = FALSE;
        dcb.fTXContinueOnXoff = TRUE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fErrorChar = FALSE;
        dcb.fNull = FALSE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fAbortOnError = FALSE;

        if (!SetCommState(handle, &dcb)) {
            throw WindowsError("configuring serial port as 4800 8N1");
        }

        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.ReadTotalTimeoutConstant = 0;
        timeouts.WriteTotalTimeoutMultiplier = 0;
        timeouts.WriteTotalTimeoutConstant = 100;
        if (!SetCommTimeouts(handle, &timeouts)) {
            throw WindowsError("configuring serial timeouts");
        }

        if (!SetupComm(handle, 4096, 4096)) {
            throw WindowsError("configuring serial buffers");
        }

        DiscardInput();
    }
    catch (...) {
        Close();
        throw;
    }
#else
    const auto handle = ::open(portName_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (handle < 0) {
        portName_.clear();
        throw PosixError("opening serial port");
    }
    handle_ = handle;

    try {
        termios options{};
        if (tcgetattr(handle_, &options) != 0) {
            throw PosixError("reading serial settings");
        }

        cfmakeraw(&options);
        if (cfsetispeed(&options, B4800) != 0 || cfsetospeed(&options, B4800) != 0) {
            throw PosixError("setting serial baud rate");
        }

        options.c_cflag &= static_cast<tcflag_t>(~(PARENB | CSTOPB | CSIZE | CRTSCTS));
        options.c_cflag |= static_cast<tcflag_t>(CS8 | CLOCAL | CREAD);
        options.c_cc[VMIN] = 0;
        options.c_cc[VTIME] = 0;

        if (tcsetattr(handle_, TCSANOW, &options) != 0) {
            throw PosixError("configuring serial port as 4800 8N1");
        }

        DiscardInput();
    }
    catch (...) {
        Close();
        throw;
    }
#endif
}

void SerialPort::Close() noexcept
{
#ifdef _WIN32
    if (IsOpen()) {
        CloseHandle(ToHandle(handle_));
        handle_ = reinterpret_cast<void*>(-1);
    }
#else
    if (IsOpen()) {
        ::close(handle_);
        handle_ = -1;
    }
#endif
    portName_.clear();
}

bool SerialPort::IsOpen() const noexcept
{
#ifdef _WIN32
    return ToHandle(handle_) != INVALID_HANDLE_VALUE;
#else
    return handle_ >= 0;
#endif
}

const std::string& SerialPort::PortName() const noexcept
{
    return portName_;
}

void SerialPort::DiscardInput()
{
    if (!IsOpen()) {
        throw std::logic_error("serial port is not open");
    }

#ifdef _WIN32
    if (!PurgeComm(ToHandle(handle_), PURGE_RXABORT | PURGE_RXCLEAR)) {
        throw WindowsError("discarding serial input");
    }
#else
    if (tcflush(handle_, TCIFLUSH) != 0) {
        throw PosixError("discarding serial input");
    }
#endif
}

void SerialPort::WriteAll(std::span<const std::uint8_t> data)
{
    if (!IsOpen()) {
        throw std::logic_error("serial port is not open");
    }

    std::size_t writtenTotal = 0;
    while (writtenTotal < data.size()) {
#ifdef _WIN32
        DWORD written = 0;
        const auto remaining = static_cast<DWORD>(data.size() - writtenTotal);
        if (!WriteFile(
                ToHandle(handle_),
                data.data() + writtenTotal,
                remaining,
                &written,
                nullptr)) {
            throw WindowsError("writing serial data");
        }
        if (written == 0) {
            throw std::runtime_error("serial write returned zero bytes");
        }
        writtenTotal += written;
#else
        const auto written = ::write(
            handle_, data.data() + writtenTotal, data.size() - writtenTotal);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            throw PosixError("writing serial data");
        }
        if (written == 0) {
            throw std::runtime_error("serial write returned zero bytes");
        }
        writtenTotal += static_cast<std::size_t>(written);
#endif
    }

#ifdef _WIN32
    if (!FlushFileBuffers(ToHandle(handle_))) {
        throw WindowsError("flushing serial output");
    }
#else
    if (tcdrain(handle_) != 0) {
        throw PosixError("flushing serial output");
    }
#endif
}

std::size_t SerialPort::ReadSome(std::span<std::uint8_t> buffer)
{
    if (!IsOpen()) {
        throw std::logic_error("serial port is not open");
    }
    if (buffer.empty()) {
        return 0;
    }

#ifdef _WIN32
    DWORD read = 0;
    const auto requested = static_cast<DWORD>(buffer.size());
    if (!ReadFile(ToHandle(handle_), buffer.data(), requested, &read, nullptr)) {
        throw WindowsError("reading serial data");
    }
    return static_cast<std::size_t>(read);
#else
    const auto read = ::read(handle_, buffer.data(), buffer.size());
    if (read < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        throw PosixError("reading serial data");
    }
    return static_cast<std::size_t>(read);
#endif
}

std::string SerialPort::NormalizePortName(std::string_view portName)
{
    if (portName.empty()) {
        throw std::invalid_argument("serial port name must not be empty");
    }

#ifdef _WIN32
    constexpr std::string_view prefix = R"(\\.\)";
    if (portName.starts_with(prefix)) {
        return std::string(portName);
    }
    return std::string(prefix) + std::string(portName);
#else
    return std::string(portName);
#endif
}

MdvSerialTransport::MdvSerialTransport(TimingSettings timing)
    : pacer_(timing)
{
}

void MdvSerialTransport::Open(std::string_view portName)
{
    port_.Open(portName);
    pacer_.Reset();
}

void MdvSerialTransport::Close() noexcept
{
    port_.Close();
    pacer_.Reset();
}

bool MdvSerialTransport::IsOpen() const noexcept
{
    return port_.IsOpen();
}

TransactionResult MdvSerialTransport::Execute(const RequestFrame& request)
{
    TransactionResult result;
    if (!port_.IsOpen()) {
        result.status = TransactionStatus::IoError;
        result.error = "serial port is not open";
        return result;
    }

    const auto startedAt = pacer_.WaitForNextStart();
    const auto deadline = pacer_.ResponseDeadline(startedAt);

    try {
        port_.DiscardInput();
        const auto wireRequest = BuildWireRequest(request);
        port_.WriteAll(wireRequest);

        ResponseFrameCollector collector;
        std::array<std::uint8_t, 64> buffer{};

        while (std::chrono::steady_clock::now() < deadline) {
            const auto count = port_.ReadSome(buffer);
            for (std::size_t index = 0; index < count; ++index) {
                if (auto response = collector.Push(buffer[index]); response.has_value()) {
                    result.status = TransactionStatus::Success;
                    result.response = *response;
                    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - startedAt);
                    return result;
                }
            }

            if (count == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        result.status = TransactionStatus::Timeout;
        result.error = "MDV response timeout";
    }
    catch (const std::exception& exception) {
        result.status = TransactionStatus::IoError;
        result.error = exception.what();
    }

    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt);
    return result;
}

const TimingSettings& MdvSerialTransport::Timing() const noexcept
{
    return pacer_.Settings();
}

const std::string& MdvSerialTransport::PortName() const noexcept
{
    return port_.PortName();
}

} // namespace mdv
