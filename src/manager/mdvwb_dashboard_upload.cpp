#include "mdvwb_dashboard_upload.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace mdvwb {
namespace {

constexpr std::array<std::uint32_t, 64> Sha256Constants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::uint32_t RotateRight(std::uint32_t value, unsigned int bits) {
    return (value >> bits) | (value << (32U - bits));
}

class Sha256 final {
public:
    void Update(std::string_view bytes) {
        for (const unsigned char byte : bytes) {
            buffer_[bufferSize_++] = byte;
            bitLength_ += 8U;
            if (bufferSize_ == buffer_.size()) {
                Transform(buffer_.data());
                bufferSize_ = 0;
            }
        }
    }

    std::array<std::uint8_t, 32> Final() {
        const std::uint64_t originalBitLength = bitLength_;
        buffer_[bufferSize_++] = 0x80U;
        if (bufferSize_ > 56U) {
            while (bufferSize_ < 64U) {
                buffer_[bufferSize_++] = 0U;
            }
            Transform(buffer_.data());
            bufferSize_ = 0;
        }
        while (bufferSize_ < 56U) {
            buffer_[bufferSize_++] = 0U;
        }
        for (int shift = 56; shift >= 0; shift -= 8) {
            buffer_[bufferSize_++] = static_cast<std::uint8_t>(
                (originalBitLength >> static_cast<unsigned int>(shift)) & 0xffU);
        }
        Transform(buffer_.data());

        std::array<std::uint8_t, 32> result{};
        for (std::size_t index = 0; index < state_.size(); ++index) {
            result[index * 4U] = static_cast<std::uint8_t>(state_[index] >> 24U);
            result[index * 4U + 1U] = static_cast<std::uint8_t>(state_[index] >> 16U);
            result[index * 4U + 2U] = static_cast<std::uint8_t>(state_[index] >> 8U);
            result[index * 4U + 3U] = static_cast<std::uint8_t>(state_[index]);
        }
        return result;
    }

private:
    void Transform(const std::uint8_t* block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16U; ++index) {
            const std::size_t offset = index * 4U;
            words[index] =
                (static_cast<std::uint32_t>(block[offset]) << 24U) |
                (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                static_cast<std::uint32_t>(block[offset + 3U]);
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            const std::uint32_t s0 = RotateRight(words[index - 15U], 7U) ^
                RotateRight(words[index - 15U], 18U) ^
                (words[index - 15U] >> 3U);
            const std::uint32_t s1 = RotateRight(words[index - 2U], 17U) ^
                RotateRight(words[index - 2U], 19U) ^
                (words[index - 2U] >> 10U);
            words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];

        for (std::size_t index = 0; index < words.size(); ++index) {
            const std::uint32_t sum1 = RotateRight(e, 6U) ^ RotateRight(e, 11U) ^
                RotateRight(e, 25U);
            const std::uint32_t choose = (e & f) ^ ((~e) & g);
            const std::uint32_t temporary1 =
                h + sum1 + choose + Sha256Constants[index] + words[index];
            const std::uint32_t sum0 = RotateRight(a, 2U) ^ RotateRight(a, 13U) ^
                RotateRight(a, 22U);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = sum0 + majority;

            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_ = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t bufferSize_ = 0;
    std::uint64_t bitLength_ = 0;
};

std::string HexDigest(const std::array<std::uint8_t, 32>& digest) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const std::uint8_t byte : digest) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

class FlatJsonParser final {
public:
    explicit FlatJsonParser(std::string_view text) : text_(text) {}

    struct Value {
        bool isString = false;
        std::string stringValue;
        std::uint64_t integerValue = 0;
    };

    std::map<std::string, Value> Parse() {
        SkipWhitespace();
        Expect('{');
        SkipWhitespace();
        std::map<std::string, Value> result;
        if (TryConsume('}')) {
            return result;
        }
        while (true) {
            const std::string key = ParseString();
            SkipWhitespace();
            Expect(':');
            SkipWhitespace();
            Value value;
            if (!AtEnd() && Peek() == '"') {
                value.isString = true;
                value.stringValue = ParseString();
            } else {
                value.integerValue = ParseUnsignedInteger();
            }
            if (!result.emplace(key, std::move(value)).second) {
                Fail("duplicate field '" + key + "'");
            }
            SkipWhitespace();
            if (TryConsume('}')) {
                break;
            }
            Expect(',');
            SkipWhitespace();
        }
        SkipWhitespace();
        if (!AtEnd()) {
            Fail("unexpected characters after object");
        }
        return result;
    }

private:
    std::string ParseString() {
        Expect('"');
        std::string result;
        while (!AtEnd()) {
            const char character = Consume();
            if (character == '"') {
                return result;
            }
            if (static_cast<unsigned char>(character) < 0x20U) {
                Fail("control character in string");
            }
            if (character != '\\') {
                result.push_back(character);
                continue;
            }
            if (AtEnd()) {
                Fail("unfinished string escape");
            }
            const char escaped = Consume();
            switch (escaped) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                default: Fail("unsupported string escape");
            }
        }
        Fail("unterminated string");
    }

    std::uint64_t ParseUnsignedInteger() {
        if (AtEnd() || std::isdigit(static_cast<unsigned char>(Peek())) == 0) {
            Fail("expected unsigned integer or string");
        }
        std::uint64_t value = 0;
        while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
            const unsigned int digit = static_cast<unsigned int>(Consume() - '0');
            if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
                Fail("integer is too large");
            }
            value = value * 10U + digit;
        }
        return value;
    }

    void SkipWhitespace() {
        while (!AtEnd()) {
            const char character = Peek();
            if (character != ' ' && character != '\t' &&
                character != '\r' && character != '\n') {
                break;
            }
            ++position_;
        }
    }

    void Expect(char expected) {
        if (AtEnd() || Consume() != expected) {
            Fail(std::string("expected '") + expected + "'");
        }
    }
    bool TryConsume(char expected) {
        if (!AtEnd() && Peek() == expected) {
            ++position_;
            return true;
        }
        return false;
    }
    char Peek() const { return text_[position_]; }
    char Consume() { return text_[position_++]; }
    bool AtEnd() const { return position_ >= text_.size(); }
    [[noreturn]] void Fail(const std::string& message) const {
        throw DashboardUploadError(
            "upload start JSON error at byte " + std::to_string(position_) +
            ": " + message);
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

const FlatJsonParser::Value& RequireField(
    const std::map<std::string, FlatJsonParser::Value>& object,
    std::string_view name) {
    const auto iterator = object.find(std::string(name));
    if (iterator == object.end()) {
        throw DashboardUploadError(
            "upload start is missing required field '" + std::string(name) + "'");
    }
    return iterator->second;
}

std::string RequireString(
    const std::map<std::string, FlatJsonParser::Value>& object,
    std::string_view name) {
    const auto& value = RequireField(object, name);
    if (!value.isString) {
        throw DashboardUploadError(
            "upload start field '" + std::string(name) + "' must be a string");
    }
    return value.stringValue;
}

std::uint64_t RequireInteger(
    const std::map<std::string, FlatJsonParser::Value>& object,
    std::string_view name) {
    const auto& value = RequireField(object, name);
    if (value.isString) {
        throw DashboardUploadError(
            "upload start field '" + std::string(name) + "' must be an integer");
    }
    return value.integerValue;
}

bool IsSafeUploadId(std::string_view value) {
    return !value.empty() && value.size() <= 64U &&
        std::all_of(value.begin(), value.end(), [](char character) {
            const unsigned char byte = static_cast<unsigned char>(character);
            return std::isalnum(byte) != 0 || character == '-' || character == '_';
        });
}

std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    });
    return value;
}

std::string ExtensionOf(std::string_view fileName) {
    const std::size_t dot = fileName.find_last_of('.');
    return dot == std::string_view::npos
        ? std::string()
        : Lowercase(std::string(fileName.substr(dot)));
}

bool IsSafeFileName(std::string_view value) {
    if (value.empty() || value.size() > 128U || value == "." || value == ".." ||
        value.find('/') != std::string_view::npos ||
        value.find('\\') != std::string_view::npos) {
        return false;
    }
    if (!std::all_of(value.begin(), value.end(), [](char character) {
            const unsigned char byte = static_cast<unsigned char>(character);
            return std::isalnum(byte) != 0 || character == '-' ||
                character == '_' || character == '.';
        })) {
        return false;
    }
    const std::string extension = ExtensionOf(value);
    return extension == ".png" || extension == ".jpg" ||
        extension == ".jpeg" || extension == ".webp";
}

bool IsHexSha256(std::string_view value) {
    return value.size() == 64U &&
        std::all_of(value.begin(), value.end(), [](char character) {
            return std::isxdigit(static_cast<unsigned char>(character)) != 0;
        });
}

std::vector<std::uint8_t> ReadBinaryFile(
    const std::filesystem::path& path,
    std::size_t expectedSize) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw DashboardUploadError("cannot read temporary upload file");
    }
    std::vector<std::uint8_t> data(expectedSize);
    if (expectedSize != 0U) {
        input.read(reinterpret_cast<char*>(data.data()),
                   static_cast<std::streamsize>(expectedSize));
    }
    if (!input || input.peek() != std::ifstream::traits_type::eof()) {
        throw DashboardUploadError("temporary upload file size is inconsistent");
    }
    return data;
}

std::uint32_t ReadBigEndian32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    return (static_cast<std::uint32_t>(data[offset]) << 24U) |
        (static_cast<std::uint32_t>(data[offset + 1U]) << 16U) |
        (static_cast<std::uint32_t>(data[offset + 2U]) << 8U) |
        static_cast<std::uint32_t>(data[offset + 3U]);
}

std::uint32_t ReadLittleEndian24(const std::vector<std::uint8_t>& data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
        (static_cast<std::uint32_t>(data[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(data[offset + 2U]) << 16U);
}

struct ImageInfo {
    std::string extension;
    int width = 0;
    int height = 0;
};

ImageInfo ParsePng(const std::vector<std::uint8_t>& data) {
    static constexpr std::array<std::uint8_t, 8> Signature = {
        0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    if (data.size() < 24U || !std::equal(Signature.begin(), Signature.end(), data.begin()) ||
        std::string_view(reinterpret_cast<const char*>(data.data() + 12U), 4U) != "IHDR") {
        throw DashboardUploadError("invalid PNG header");
    }
    const std::uint32_t width = ReadBigEndian32(data, 16U);
    const std::uint32_t height = ReadBigEndian32(data, 20U);
    if (width == 0U || height == 0U || width > 8192U || height > 8192U) {
        throw DashboardUploadError("PNG dimensions must be in range 1..8192");
    }
    return ImageInfo{".png", static_cast<int>(width), static_cast<int>(height)};
}

ImageInfo ParseJpeg(const std::vector<std::uint8_t>& data) {
    if (data.size() < 4U || data[0] != 0xffU || data[1] != 0xd8U) {
        throw DashboardUploadError("invalid JPEG header");
    }
    std::size_t position = 2U;
    while (position + 4U <= data.size()) {
        while (position < data.size() && data[position] == 0xffU) {
            ++position;
        }
        if (position >= data.size()) {
            break;
        }
        const std::uint8_t marker = data[position++];
        if (marker == 0xd9U || marker == 0xdaU) {
            break;
        }
        if (marker == 0x01U || (marker >= 0xd0U && marker <= 0xd7U)) {
            continue;
        }
        if (position + 2U > data.size()) {
            break;
        }
        const std::size_t segmentLength =
            (static_cast<std::size_t>(data[position]) << 8U) |
            static_cast<std::size_t>(data[position + 1U]);
        if (segmentLength < 2U || position + segmentLength > data.size()) {
            throw DashboardUploadError("invalid JPEG segment length");
        }
        const bool startOfFrame =
            marker == 0xc0U || marker == 0xc1U || marker == 0xc2U || marker == 0xc3U ||
            marker == 0xc5U || marker == 0xc6U || marker == 0xc7U || marker == 0xc9U ||
            marker == 0xcaU || marker == 0xcbU || marker == 0xcdU || marker == 0xceU ||
            marker == 0xcfU;
        if (startOfFrame) {
            if (segmentLength < 7U) {
                throw DashboardUploadError("JPEG start-of-frame segment is too short");
            }
            const int height =
                (static_cast<int>(data[position + 3U]) << 8U) |
                static_cast<int>(data[position + 4U]);
            const int width =
                (static_cast<int>(data[position + 5U]) << 8U) |
                static_cast<int>(data[position + 6U]);
            if (width < 1 || height < 1 || width > 8192 || height > 8192) {
                throw DashboardUploadError("JPEG dimensions must be in range 1..8192");
            }
            return ImageInfo{".jpg", width, height};
        }
        position += segmentLength;
    }
    throw DashboardUploadError("JPEG dimensions were not found");
}

ImageInfo ParseWebp(const std::vector<std::uint8_t>& data) {
    if (data.size() < 30U ||
        std::string_view(reinterpret_cast<const char*>(data.data()), 4U) != "RIFF" ||
        std::string_view(reinterpret_cast<const char*>(data.data() + 8U), 4U) != "WEBP") {
        throw DashboardUploadError("invalid WebP header");
    }
    const std::string_view type(reinterpret_cast<const char*>(data.data() + 12U), 4U);
    int width = 0;
    int height = 0;
    if (type == "VP8X") {
        width = static_cast<int>(ReadLittleEndian24(data, 24U) + 1U);
        height = static_cast<int>(ReadLittleEndian24(data, 27U) + 1U);
    } else if (type == "VP8L") {
        if (data.size() < 25U || data[20U] != 0x2fU) {
            throw DashboardUploadError("invalid lossless WebP header");
        }
        width = 1 + static_cast<int>(data[21U] | ((data[22U] & 0x3fU) << 8U));
        height = 1 + static_cast<int>(((data[22U] & 0xc0U) >> 6U) |
            (static_cast<unsigned int>(data[23U]) << 2U) |
            ((static_cast<unsigned int>(data[24U]) & 0x0fU) << 10U));
    } else if (type == "VP8 ") {
        if (data.size() < 30U || data[23U] != 0x9dU ||
            data[24U] != 0x01U || data[25U] != 0x2aU) {
            throw DashboardUploadError("invalid lossy WebP header");
        }
        width = static_cast<int>((data[26U] | (data[27U] << 8U)) & 0x3fffU);
        height = static_cast<int>((data[28U] | (data[29U] << 8U)) & 0x3fffU);
    } else {
        throw DashboardUploadError("unsupported WebP encoding");
    }
    if (width < 1 || height < 1 || width > 8192 || height > 8192) {
        throw DashboardUploadError("WebP dimensions must be in range 1..8192");
    }
    return ImageInfo{".webp", width, height};
}

ImageInfo ParseImage(const std::vector<std::uint8_t>& data) {
    if (data.size() >= 8U && data[0] == 0x89U && data[1] == 0x50U) {
        return ParsePng(data);
    }
    if (data.size() >= 2U && data[0] == 0xffU && data[1] == 0xd8U) {
        return ParseJpeg(data);
    }
    if (data.size() >= 12U &&
        std::string_view(reinterpret_cast<const char*>(data.data()), 4U) == "RIFF") {
        return ParseWebp(data);
    }
    throw DashboardUploadError("file is not a supported PNG, JPEG or WebP image");
}

}  // namespace

DashboardUploadStartRequest ParseDashboardUploadStart(std::string_view payload) {
    const auto object = FlatJsonParser(payload).Parse();
    static constexpr std::array<std::string_view, 7> Allowed = {
        "version", "uploadId", "fileName", "panelId", "size", "sha256", "revision"};
    for (const auto& [key, unused] : object) {
        static_cast<void>(unused);
        if (std::find(Allowed.begin(), Allowed.end(), key) == Allowed.end()) {
            throw DashboardUploadError("upload start contains unknown field '" + key + "'");
        }
    }

    DashboardUploadStartRequest request;
    const std::uint64_t version = RequireInteger(object, "version");
    const std::uint64_t size = RequireInteger(object, "size");
    const std::uint64_t revision = RequireInteger(object, "revision");
    if (version != 1U) {
        throw DashboardUploadError("upload start version must be 1");
    }
    if (size == 0U || size > DashboardBackgroundUpload::MaximumFileBytes) {
        throw DashboardUploadError("upload size must be in range 1..10485760 bytes");
    }
    if (revision > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        throw DashboardUploadError("upload revision is outside the supported range");
    }

    request.version = 1;
    request.uploadId = RequireString(object, "uploadId");
    request.fileName = RequireString(object, "fileName");
    const auto panel = object.find("panelId");
    request.panelId = panel == object.end() ? "main" : RequireString(object, "panelId");
    request.size = static_cast<std::size_t>(size);
    request.sha256 = Lowercase(RequireString(object, "sha256"));
    request.revision = static_cast<int>(revision);

    if (!IsSafeUploadId(request.uploadId)) {
        throw DashboardUploadError(
            "uploadId must contain 1..64 ASCII letters, digits, '-' or '_'");
    }
    if (!IsSafeUploadId(request.panelId) || request.panelId.size() > 48U) {
        throw DashboardUploadError(
            "panelId must contain 1..48 ASCII letters, digits, '-' or '_'");
    }
    if (!IsSafeFileName(request.fileName)) {
        throw DashboardUploadError(
            "fileName must be a safe PNG, JPEG or WebP base name");
    }
    if (!IsHexSha256(request.sha256)) {
        throw DashboardUploadError("sha256 must contain exactly 64 hexadecimal characters");
    }
    return request;
}

std::string ComputeSha256Hex(std::string_view bytes) {
    Sha256 hash;
    hash.Update(bytes);
    return HexDigest(hash.Final());
}

DashboardBackgroundUpload::DashboardBackgroundUpload(
    std::filesystem::path assetDirectory)
    : assetDirectory_(std::move(assetDirectory)) {
    if (assetDirectory_.empty()) {
        throw std::invalid_argument("dashboard asset directory cannot be empty");
    }
}

DashboardBackgroundUpload::~DashboardBackgroundUpload() {
    try {
        Cancel();
    } catch (...) {
    }
}

void DashboardBackgroundUpload::Start(const DashboardUploadStartRequest& request) {
    Cancel();
    std::error_code error;
    std::filesystem::create_directories(assetDirectory_, error);
    if (error) {
        throw DashboardUploadError(
            "cannot create dashboard asset directory: " + error.message());
    }
    temporaryPath_ = assetDirectory_ / (".upload-" + request.uploadId + ".part");
    std::ofstream output(temporaryPath_, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw DashboardUploadError("cannot create temporary upload file");
    }
    request_ = request;
    receivedBytes_ = 0;
    nextChunkIndex_ = 0;
}

void DashboardBackgroundUpload::Append(
    std::string_view uploadId,
    std::size_t index,
    std::string_view bytes) {
    if (!request_.has_value()) {
        throw DashboardUploadError("no dashboard background upload is active");
    }
    if (uploadId != request_->uploadId) {
        throw DashboardUploadError("uploadId does not match the active upload");
    }
    if (index != nextChunkIndex_) {
        throw DashboardUploadError(
            "chunk index must be " + std::to_string(nextChunkIndex_));
    }
    if (bytes.empty() || bytes.size() > MaximumChunkBytes) {
        throw DashboardUploadError("chunk size must be in range 1..49152 bytes");
    }
    if (receivedBytes_ > request_->size ||
        bytes.size() > request_->size - receivedBytes_) {
        throw DashboardUploadError("chunk exceeds the declared upload size");
    }

    std::ofstream output(temporaryPath_, std::ios::binary | std::ios::app);
    if (!output) {
        throw DashboardUploadError("cannot open temporary upload file");
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw DashboardUploadError("cannot write dashboard upload chunk");
    }
    receivedBytes_ += bytes.size();
    ++nextChunkIndex_;
}

DashboardPreparedAsset DashboardBackgroundUpload::Prepare(
    std::string_view uploadId) const {
    if (!request_.has_value()) {
        throw DashboardUploadError("no dashboard background upload is active");
    }
    if (uploadId != request_->uploadId) {
        throw DashboardUploadError("uploadId does not match the active upload");
    }
    if (receivedBytes_ != request_->size) {
        throw DashboardUploadError(
            "upload is incomplete: received " + std::to_string(receivedBytes_) +
            " of " + std::to_string(request_->size) + " bytes");
    }

    const std::vector<std::uint8_t> data = ReadBinaryFile(temporaryPath_, request_->size);
    const std::string_view bytes(
        reinterpret_cast<const char*>(data.data()), data.size());
    const std::string actualSha256 = ComputeSha256Hex(bytes);
    if (actualSha256 != request_->sha256) {
        throw DashboardUploadError("uploaded file SHA-256 does not match the declaration");
    }
    const ImageInfo image = ParseImage(data);
    const std::string submittedExtension = ExtensionOf(request_->fileName);
    const bool jpegMatch = image.extension == ".jpg" &&
        (submittedExtension == ".jpg" || submittedExtension == ".jpeg");
    if (!jpegMatch && submittedExtension != image.extension) {
        throw DashboardUploadError("file extension does not match the detected image format");
    }

    DashboardPreparedAsset result;
    result.uploadId = request_->uploadId;
    result.originalFileName = request_->fileName;
    result.panelId = request_->panelId;
    result.sha256 = actualSha256;
    result.size = request_->size;
    result.width = image.width;
    result.height = image.height;
    result.expectedRevision = request_->revision;
    result.finalFileName = "background-" + actualSha256.substr(0, 16U) + image.extension;
    result.temporaryPath = temporaryPath_;
    result.finalPath = assetDirectory_ / result.finalFileName;
    return result;
}

void DashboardBackgroundUpload::Cancel(std::string_view uploadId) {
    if (!request_.has_value()) {
        return;
    }
    if (!uploadId.empty() && uploadId != request_->uploadId) {
        throw DashboardUploadError("uploadId does not match the active upload");
    }
    std::error_code error;
    std::filesystem::remove(temporaryPath_, error);
    request_.reset();
    temporaryPath_.clear();
    receivedBytes_ = 0;
    nextChunkIndex_ = 0;
}

void DashboardBackgroundUpload::Release() noexcept {
    request_.reset();
    temporaryPath_.clear();
    receivedBytes_ = 0;
    nextChunkIndex_ = 0;
}

bool DashboardBackgroundUpload::Active() const noexcept { return request_.has_value(); }
std::string_view DashboardBackgroundUpload::UploadId() const noexcept {
    return request_.has_value() ? std::string_view(request_->uploadId) : std::string_view{};
}
std::string_view DashboardBackgroundUpload::FileName() const noexcept {
    return request_.has_value() ? std::string_view(request_->fileName) : std::string_view{};
}

std::string_view DashboardBackgroundUpload::PanelId() const noexcept {
    return request_.has_value() ? std::string_view(request_->panelId) : std::string_view{};
}
std::size_t DashboardBackgroundUpload::ExpectedBytes() const noexcept {
    return request_.has_value() ? request_->size : 0U;
}
std::size_t DashboardBackgroundUpload::ReceivedBytes() const noexcept {
    return receivedBytes_;
}
std::size_t DashboardBackgroundUpload::NextChunkIndex() const noexcept {
    return nextChunkIndex_;
}
int DashboardBackgroundUpload::ExpectedRevision() const noexcept {
    return request_.has_value() ? request_->revision : 0;
}
const std::filesystem::path& DashboardBackgroundUpload::AssetDirectory() const noexcept {
    return assetDirectory_;
}

}  // namespace mdvwb
