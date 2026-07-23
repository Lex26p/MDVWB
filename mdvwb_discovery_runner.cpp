#include "mdvwb_discovery_runner.h"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <set>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mdvwb {
namespace {

constexpr std::string_view ResultPrefix = "FOUND_ADDRESSES=";

std::string TrimLineEnd(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
    return value;
}

std::string LastNonEmptyLine(std::string_view output) {
    std::istringstream input{std::string(output)};
    std::string line;
    std::string last;
    while (std::getline(input, line)) {
        line = TrimLineEnd(std::move(line));
        if (!line.empty()) {
            last = line;
        }
    }
    return last;
}

#ifndef _WIN32
std::pair<int, std::string> ExecuteAndCapture(
    const std::string& executable,
    const std::vector<std::string>& arguments) {
    int pipeDescriptors[2]{};
    if (pipe(pipeDescriptors) != 0) {
        throw std::runtime_error(
            "cannot create discovery output pipe: " + std::string(std::strerror(errno)));
    }

    const pid_t child = fork();
    if (child < 0) {
        close(pipeDescriptors[0]);
        close(pipeDescriptors[1]);
        throw std::runtime_error(
            "cannot start discovery process: " + std::string(std::strerror(errno)));
    }

    if (child == 0) {
        close(pipeDescriptors[0]);
        if (dup2(pipeDescriptors[1], STDOUT_FILENO) < 0 ||
            dup2(pipeDescriptors[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(pipeDescriptors[1]);

        std::vector<char*> argv;
        argv.reserve(arguments.size() + 2U);
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execv(executable.c_str(), argv.data());
        _exit(127);
    }

    close(pipeDescriptors[1]);
    std::string output;
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(pipeDescriptors[0], buffer, sizeof(buffer));
        if (count > 0) {
            output.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        close(pipeDescriptors[0]);
        throw std::runtime_error(
            "cannot read discovery output: " + std::string(std::strerror(errno)));
    }
    close(pipeDescriptors[0]);

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        throw std::runtime_error(
            "cannot wait for discovery process: " + std::string(std::strerror(errno)));
    }

    if (WIFEXITED(status)) {
        return {WEXITSTATUS(status), std::move(output)};
    }
    if (WIFSIGNALED(status)) {
        return {128 + WTERMSIG(status), std::move(output)};
    }
    return {125, std::move(output)};
}
#endif

}  // namespace

std::vector<int> ParseDiscoveryAddresses(std::string_view output) {
    std::istringstream input{std::string(output)};
    std::string line;
    std::optional<std::string> value;
    while (std::getline(input, line)) {
        line = TrimLineEnd(std::move(line));
        if (line.rfind(ResultPrefix, 0) == 0U) {
            value = line.substr(ResultPrefix.size());
        }
    }
    if (!value.has_value()) {
        throw std::runtime_error("discovery output does not contain FOUND_ADDRESSES");
    }
    if (value->empty()) {
        return {};
    }

    std::set<int> unique;
    std::size_t begin = 0;
    while (begin <= value->size()) {
        const std::size_t separator = value->find(',', begin);
        const std::size_t end = separator == std::string::npos ? value->size() : separator;
        if (end == begin) {
            throw std::runtime_error("discovery output contains an empty address");
        }
        const std::string_view token(value->data() + begin, end - begin);
        int address = -1;
        const auto parsed = std::from_chars(
            token.data(), token.data() + token.size(), address);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
            address < 0 || address > 63) {
            throw std::runtime_error("discovery output contains an invalid address");
        }
        if (!unique.insert(address).second) {
            throw std::runtime_error("discovery output contains a duplicate address");
        }
        if (separator == std::string::npos) {
            break;
        }
        begin = separator + 1U;
    }

    return {unique.begin(), unique.end()};
}

NativeDiscoveryRunner::NativeDiscoveryRunner(std::string executablePath)
    : executablePath_(std::move(executablePath)) {
    if (executablePath_.empty()) {
        throw std::invalid_argument("discovery executable path cannot be empty");
    }
}

DiscoveryExecutionResult NativeDiscoveryRunner::Run(
    std::string_view port,
    int masterId,
    int periodMilliseconds,
    int responseTimeoutMilliseconds) {
    if (port.empty()) {
        throw std::invalid_argument("discovery port cannot be empty");
    }
    if (masterId < 0 || masterId > 63) {
        throw std::invalid_argument("discovery master id must be in range 0..63");
    }
    if (periodMilliseconds <= 0 || responseTimeoutMilliseconds <= 0) {
        throw std::invalid_argument("discovery timing must be positive");
    }

#ifdef _WIN32
    throw std::runtime_error("native discovery process is supported only on Linux");
#else
    const std::vector<std::string> arguments{
        "--discover",
        "--port", std::string(port),
        "--master-id", std::to_string(masterId),
        "--period-ms", std::to_string(periodMilliseconds),
        "--response-timeout-ms", std::to_string(responseTimeoutMilliseconds),
    };
    auto [exitCode, output] = ExecuteAndCapture(executablePath_, arguments);

    DiscoveryExecutionResult result;
    result.exitCode = exitCode;
    result.output = std::move(output);
    if (exitCode != 0) {
        result.message = LastNonEmptyLine(result.output);
        if (result.message.empty()) {
            result.message = "Discovery process exited with code " + std::to_string(exitCode);
        }
        return result;
    }

    try {
        result.addresses = ParseDiscoveryAddresses(result.output);
        result.success = true;
        result.message = result.addresses.empty()
            ? "Discovery completed; no devices found"
            : "Discovery completed";
    } catch (const std::exception& error) {
        result.message = error.what();
    }
    return result;
#endif
}

}  // namespace mdvwb
