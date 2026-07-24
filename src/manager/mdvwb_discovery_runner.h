#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mdvwb {

struct DiscoveryExecutionResult {
    bool success = false;
    int exitCode = -1;
    std::vector<int> addresses;
    std::string output;
    std::string message;
};

class DiscoveryRunner {
public:
    virtual ~DiscoveryRunner() = default;
    [[nodiscard]] virtual DiscoveryExecutionResult Run(
        std::string_view port,
        int masterId,
        int periodMilliseconds,
        int responseTimeoutMilliseconds) = 0;
};

class NativeDiscoveryRunner final : public DiscoveryRunner {
public:
    explicit NativeDiscoveryRunner(std::string executablePath);

    [[nodiscard]] DiscoveryExecutionResult Run(
        std::string_view port,
        int masterId,
        int periodMilliseconds,
        int responseTimeoutMilliseconds) override;

private:
    std::string executablePath_;
};

[[nodiscard]] std::vector<int> ParseDiscoveryAddresses(std::string_view output);

}  // namespace mdvwb
