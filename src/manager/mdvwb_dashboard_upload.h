#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mdvwb {

struct DashboardUploadStartRequest {
    int version = 1;
    std::string uploadId;
    std::string fileName;
    std::string panelId = "main";
    std::size_t size = 0;
    std::string sha256;
    int revision = 0;
};

struct DashboardPreparedAsset {
    std::string uploadId;
    std::string originalFileName;
    std::string panelId;
    std::string finalFileName;
    std::string sha256;
    std::size_t size = 0;
    int width = 0;
    int height = 0;
    int expectedRevision = 0;
    std::filesystem::path temporaryPath;
    std::filesystem::path finalPath;
};

class DashboardUploadError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

DashboardUploadStartRequest ParseDashboardUploadStart(std::string_view payload);
std::string ComputeSha256Hex(std::string_view bytes);

class DashboardBackgroundUpload final {
public:
    static constexpr std::size_t MaximumFileBytes = 10U * 1024U * 1024U;
    static constexpr std::size_t MaximumChunkBytes = 48U * 1024U;

    explicit DashboardBackgroundUpload(std::filesystem::path assetDirectory);
    ~DashboardBackgroundUpload();

    DashboardBackgroundUpload(const DashboardBackgroundUpload&) = delete;
    DashboardBackgroundUpload& operator=(const DashboardBackgroundUpload&) = delete;

    void Start(const DashboardUploadStartRequest& request);
    void Append(std::string_view uploadId, std::size_t index, std::string_view bytes);
    [[nodiscard]] DashboardPreparedAsset Prepare(std::string_view uploadId) const;
    void Cancel(std::string_view uploadId = {});
    void Release() noexcept;

    [[nodiscard]] bool Active() const noexcept;
    [[nodiscard]] std::string_view UploadId() const noexcept;
    [[nodiscard]] std::string_view FileName() const noexcept;
    [[nodiscard]] std::string_view PanelId() const noexcept;
    [[nodiscard]] std::size_t ExpectedBytes() const noexcept;
    [[nodiscard]] std::size_t ReceivedBytes() const noexcept;
    [[nodiscard]] std::size_t NextChunkIndex() const noexcept;
    [[nodiscard]] int ExpectedRevision() const noexcept;
    [[nodiscard]] const std::filesystem::path& AssetDirectory() const noexcept;

private:
    std::filesystem::path assetDirectory_;
    std::optional<DashboardUploadStartRequest> request_;
    std::filesystem::path temporaryPath_;
    std::size_t receivedBytes_ = 0;
    std::size_t nextChunkIndex_ = 0;
};

}  // namespace mdvwb
