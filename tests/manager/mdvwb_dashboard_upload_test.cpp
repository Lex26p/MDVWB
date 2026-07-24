#include "mdvwb_dashboard_upload.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("mdvwb-dashboard-upload-test-" + std::to_string(token));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path path_;
};

std::string PngHeader(int width, int height) {
    std::string data(32, '\0');
    const unsigned char signature[] = {0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    for (std::size_t index = 0; index < 8U; ++index) {
        data[index] = static_cast<char>(signature[index]);
    }
    data[8] = 0;
    data[9] = 0;
    data[10] = 0;
    data[11] = 13;
    data.replace(12, 4, "IHDR");
    data[16] = static_cast<char>((width >> 24) & 0xff);
    data[17] = static_cast<char>((width >> 16) & 0xff);
    data[18] = static_cast<char>((width >> 8) & 0xff);
    data[19] = static_cast<char>(width & 0xff);
    data[20] = static_cast<char>((height >> 24) & 0xff);
    data[21] = static_cast<char>((height >> 16) & 0xff);
    data[22] = static_cast<char>((height >> 8) & 0xff);
    data[23] = static_cast<char>(height & 0xff);
    return data;
}

void TestSha256() {
    Require(
        mdvwb::ComputeSha256Hex("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA-256 implementation returned the wrong digest");
}

void TestStartPayloadParsing() {
    const auto request = mdvwb::ParseDashboardUploadStart(
        R"json({"version":1,"uploadId":"upload_42","fileName":"floor.webp","size":1234,"sha256":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","revision":7})json");
    Require(request.uploadId == "upload_42", "uploadId was parsed incorrectly");
    Require(request.fileName == "floor.webp", "fileName was parsed incorrectly");
    Require(request.size == 1234U, "file size was parsed incorrectly");
    Require(request.revision == 7, "revision was parsed incorrectly");
    Require(request.panelId == "main", "missing panelId must default to main");
    Require(request.sha256 == std::string(64U, 'a'), "SHA-256 was not normalized");

    bool rejected = false;
    try {
        static_cast<void>(mdvwb::ParseDashboardUploadStart(
            R"json({"version":1,"uploadId":"../bad","fileName":"floor.svg","size":1,"sha256":"00","revision":0})json"));
    } catch (const mdvwb::DashboardUploadError&) {
        rejected = true;
    }
    Require(rejected, "unsafe upload start payload was accepted");

    const auto panelRequest = mdvwb::ParseDashboardUploadStart(
        R"json({"version":1,"uploadId":"upload_43","fileName":"floor.webp","panelId":"floor-2","size":1234,"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","revision":8})json");
    Require(panelRequest.panelId == "floor-2", "panelId was parsed incorrectly");

    rejected = false;
    try {
        static_cast<void>(mdvwb::ParseDashboardUploadStart(
            R"json({"version":1,"uploadId":"upload_44","fileName":"floor.webp","panelId":"../admin","size":1234,"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","revision":8})json"));
    } catch (const mdvwb::DashboardUploadError&) {
        rejected = true;
    }
    Require(rejected, "unsafe panelId was accepted");
}

void TestPngUploadAndSequentialChunks() {
    TemporaryDirectory temporary;
    const std::string image = PngHeader(2400, 1600);
    mdvwb::DashboardUploadStartRequest request;
    request.uploadId = "png-test";
    request.fileName = "floor-plan.png";
    request.panelId = "floor-2";
    request.size = image.size();
    request.sha256 = mdvwb::ComputeSha256Hex(image);
    request.revision = 3;

    mdvwb::DashboardBackgroundUpload upload(temporary.Path() / "assets");
    upload.Start(request);
    Require(upload.Active(), "upload did not become active");
    upload.Append(request.uploadId, 0, std::string_view(image).substr(0, 12));

    bool rejected = false;
    try {
        upload.Append(request.uploadId, 2, std::string_view(image).substr(12));
    } catch (const mdvwb::DashboardUploadError&) {
        rejected = true;
    }
    Require(rejected, "out-of-order chunk was accepted");

    upload.Append(request.uploadId, 1, std::string_view(image).substr(12));
    const auto prepared = upload.Prepare(request.uploadId);
    Require(prepared.width == 2400 && prepared.height == 1600,
            "PNG dimensions were parsed incorrectly");
    Require(prepared.finalFileName.rfind("background-", 0) == 0,
            "content-addressed file name is missing");
    Require(prepared.finalFileName.ends_with(".png"),
            "PNG final extension is wrong");
    Require(prepared.expectedRevision == 3,
            "expected revision was not preserved");
    Require(prepared.panelId == "floor-2",
            "prepared asset did not preserve panelId");
    Require(std::filesystem::exists(prepared.temporaryPath),
            "temporary upload file is missing");
}

void TestShaMismatchIsRejected() {
    TemporaryDirectory temporary;
    const std::string image = PngHeader(100, 100);
    mdvwb::DashboardUploadStartRequest request;
    request.uploadId = "bad-sha";
    request.fileName = "plan.png";
    request.size = image.size();
    request.sha256 = std::string(64U, '0');

    mdvwb::DashboardBackgroundUpload upload(temporary.Path() / "assets");
    upload.Start(request);
    upload.Append(request.uploadId, 0, image);
    bool rejected = false;
    try {
        static_cast<void>(upload.Prepare(request.uploadId));
    } catch (const mdvwb::DashboardUploadError&) {
        rejected = true;
    }
    Require(rejected, "SHA-256 mismatch was accepted");
}

}  // namespace

int main() {
    try {
        TestSha256();
        TestStartPayloadParsing();
        TestPngUploadAndSequentialChunks();
        TestShaMismatchIsRejected();
        std::cout << "MDVWB dashboard upload tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MDVWB dashboard upload tests: FAILED: " << error.what() << '\n';
        return 1;
    }
}
