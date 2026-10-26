#include "import/ImageLoader.h"

#include <algorithm>

#include "core/Log.h"

// stb_image is a single-header library: exactly one .cpp file defines the
// implementation. Only the formats we expect are compiled in.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_HDR     // Radiance .hdr: environment maps (read as 8-bit it is tone-clamped, see loadImageHdr)
#define STBI_NO_STDIO     // we read the file ourselves (see below)
#include <stb_image.h>

#include <fstream>
#include <iterator>

namespace fe {
namespace {

/// Largest side accepted. 16384 px is also the usual GPU limit, and it keeps
/// a corrupt header from asking for gigabytes.
constexpr int kMaxSide = 16384;

} // namespace

std::optional<ImageData> decodeImage(const uint8_t* bytes, size_t size, const std::string& label) {
    if (!bytes || size == 0) {
        FE_ERROR("ImageLoader: '%s' is empty", label.c_str());
        return std::nullopt;
    }

    int width = 0, height = 0, channelsInFile = 0;
    unsigned char* decoded = stbi_load_from_memory(
        bytes, static_cast<int>(size),
        &width, &height, &channelsInFile, 4);   // 4: force RGBA

    if (!decoded) {
        FE_ERROR("ImageLoader: cannot decode '%s': %s", label.c_str(), stbi_failure_reason());
        return std::nullopt;
    }

    if (width <= 0 || height <= 0 || width > kMaxSide || height > kMaxSide) {
        FE_ERROR("ImageLoader: '%s' has an unsupported size (%d x %d)",
                 label.c_str(), width, height);
        stbi_image_free(decoded);
        return std::nullopt;
    }

    ImageData image;
    image.width  = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    image.pixels.assign(decoded, decoded + static_cast<size_t>(width) * height * 4);
    stbi_image_free(decoded);
    return image;
}

std::optional<ImageData> loadImage(const std::filesystem::path& path) {
    // stb's own file loading goes through fopen(), which cannot open a path
    // with non-ASCII characters on Windows. std::ifstream with a
    // std::filesystem::path can, so we read the bytes and decode from memory.
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        FE_ERROR("ImageLoader: cannot open '%s'", path.string().c_str());
        return std::nullopt;
    }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),
                                           std::istreambuf_iterator<char>());
    return decodeImage(bytes.data(), bytes.size(), path.string());
}

std::optional<HdrImageData> loadImageHdr(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        FE_ERROR("ImageLoader: cannot open '%s'", path.string().c_str());
        return std::nullopt;
    }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),
                                           std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        FE_ERROR("ImageLoader: '%s' is empty", path.string().c_str());
        return std::nullopt;
    }

    int width = 0, height = 0, channelsInFile = 0;
    float* decoded = stbi_loadf_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                            &width, &height, &channelsInFile, 3);
    if (!decoded) {
        FE_ERROR("ImageLoader: cannot decode '%s': %s", path.string().c_str(), stbi_failure_reason());
        return std::nullopt;
    }
    if (width <= 0 || height <= 0 || width > kMaxSide || height > kMaxSide) {
        FE_ERROR("ImageLoader: '%s' has an unsupported size (%d x %d)",
                 path.string().c_str(), width, height);
        stbi_image_free(decoded);
        return std::nullopt;
    }

    HdrImageData image;
    image.width  = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    image.pixels.assign(decoded, decoded + static_cast<size_t>(width) * height * 3);
    stbi_image_free(decoded);
    return image;
}

ImageData shrunkToFit(ImageData image, uint32_t maxSide) {
    if (maxSide == 0 || image.width == 0 || image.height == 0) return image;
    const uint32_t longest = std::max(image.width, image.height);
    if (longest <= maxSide) return image;

    // The smallest whole factor that makes the longest side fit.
    const uint32_t factor = (longest + maxSide - 1) / maxSide;
    const uint32_t width  = std::max(image.width / factor, 1u);
    const uint32_t height = std::max(image.height / factor, 1u);

    ImageData result;
    result.width  = width;
    result.height = height;
    result.pixels.resize(static_cast<size_t>(width) * height * 4);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t sum[4] = {0, 0, 0, 0};
            for (uint32_t dy = 0; dy < factor; ++dy) {
                for (uint32_t dx = 0; dx < factor; ++dx) {
                    const uint8_t* p = &image.pixels[(static_cast<size_t>(y * factor + dy) * image.width
                                                      + (x * factor + dx)) * 4];
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
                }
            }
            uint8_t* out = &result.pixels[(static_cast<size_t>(y) * width + x) * 4];
            const uint32_t count = factor * factor;
            for (int c = 0; c < 4; ++c) out[c] = static_cast<uint8_t>((sum[c] + count / 2) / count);
        }
    }
    return result;
}

std::optional<ImageData> loadImage(const ImageSource& source) {
    if (source.encoded)
        return decodeImage(source.encoded->data(), source.encoded->size(), source.label);
    return loadImage(source.file);
}

} // namespace fe
