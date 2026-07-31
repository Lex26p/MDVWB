#include "serial_port.h"

#include <cerrno>
#include <chrono>
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

[[nodiscard]] bool IsSupportedBaudRate(std::uint32_t baudRate) noexcept
{
    switch (baudRate) {
    case 1200:
    case 2400:
    case 4800:
    case 9600:
    case 19200:
    case 38400:
    case 57600:
    case 115200:
        return true;
    default:
        return false;
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

[[nodiscard]] speed_t PosixBaudRate(std::uint32_t baudRate)
{
    switch (baudRate) {
    case 1200:
        return B1200;
    case 2400:
        return B2400;
    case 4800:
        return B4800;
    case 9600:
        return B9600;
    case 19200:
        return B19200;
    case 38400:
        return B38400;
#ifdef B57600
    case 57600:
        return B57600;
#endif
#ifdef B115200
    case 115200:
        return B115200;
#endif
    default:
        throw std::invalid_argument("serial baud rate is not supported on this platform");
    }
}

#endif

} // namespace

void ValidateSerialSettings(const SerialSettings& settings)
{
    if (!IsSupportedBaudRate(settings.baudRate)) {
        throw std::invalid_argument(
            "serial baud rate must be one of 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200");
    }
    if (settings.dataBits != 7 && settings.dataBits != 8) {
        throw std::invalid_argument("serial data bits must be 7 or 8");
    }
    if (settings.stopBits != 1 && settings.stopBits != 2) {
        throw std::invalid_argument("serial stop bits must be 1 or 2");
    }
}

SerialPort::~SerialPort()
{
    Close();
}

SerialPort::SerialPort(SerialPort&& other) noexcept
    : handle_(other.handle_),
      portName_(std::move(other.portName_)),
      settings_(other.settings_)
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
        settings_ = other.settings_;
#ifdef _WIN32
        other.handle_ = reinterpret_cast<void*>(-1);
#else
        other.handle_ = -1;
#endif
    }
    return *this;
}

void SerialPort::Open(std::string_view portName, SerialSettings settings)
{
    ValidateSerialSettings(settings);
    Close();
    portName_ = NormalizePortName(portName);
    settings_ = settings;

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

        dcb.BaudRate = settings.baudRate;
        dcb.ByteSize = settings.dataBits;
        switch (settings.parity) {
        case SerialParity::None:
            dcb.Parity = NOPARITY;
            dcb.fParity = FALSE;
            break;
        case SerialParity::Even:
            dcb.Parity = EVENPARITY;
            dcb.fParity = TRUE;
            break;
        case SerialParity::Odd:
            dcb.Parity = ODDPARITY;
            dcb.fParity = TRUE;
            break;
        }
        dcb.StopBits = settings.stopBits == 1 ? ONESTOPBIT : TWOSTOPBITS;
        dcb.fBinary = TRUE;
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
            throw WindowsError("configuring serial port");
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
        const auto speed = PosixBaudRate(settings.baudRate);
        if (cfsetispeed(&options, speed) != 0 || cfsetospeed(&options, speed) != 0) {
            throw PosixError("setting serial baud rate");
        }

        options.c_cflag &= static_cast<tcflag_t>(~(PARENB | PARODD | CSTOPB | CSIZE | CRTSCTS));
        options.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD);
        options.c_cflag |= settings.dataBits == 7 ? CS7 : CS8;
        if (settings.parity == SerialParity::Even) {
            options.c_cflag |= PARENB;
        }
        else if (settings.parity == SerialParity::Odd) {
            options.c_cflag |= static_cast<tcflag_t>(PARENB | PARODD);
        }
        if (settings.stopBits == 2) {
            options.c_cflag |= CSTOPB;
        }
        options.c_cc[VMIN] = 0;
        options.c_cc[VTIME] = 0;

        if (tcsetattr(handle_, TCSANOW, &options) != 0) {
            throw PosixError("configuring serial port");
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

const SerialSettings& SerialPort::Settings() const noexcept
{
    return settings_;
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

} // namespace mdv
