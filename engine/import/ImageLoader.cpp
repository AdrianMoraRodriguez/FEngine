#include "import/ImageLoader.h"

#include "core/Log.h"

// stb_image is a single-header library: exactly one .cpp file defines the
// implementation. Only the formats we expect are compiled in.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
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
    if (bytes.empty()) {
        FE_ERROR("ImageLoader: '%s' is empty", path.string().c_str());
        return std::nullopt;
    }

    int width = 0, height = 0, channelsInFile = 0;
    unsigned char* decoded = stbi_load_from_memory(
        bytes.data(), static_cast<int>(bytes.size()),
        &width, &height, &channelsInFile, 4);   // 4: force RGBA

    if (!decoded) {
        FE_ERROR("ImageLoader: cannot decode '%s': %s",
                 path.string().c_str(), stbi_failure_reason());
        return std::nullopt;
    }

    if (width <= 0 || height <= 0 || width > kMaxSide || height > kMaxSide) {
        FE_ERROR("ImageLoader: '%s' has an unsupported size (%d x %d)",
                 path.string().c_str(), width, height);
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

} // namespace fe
