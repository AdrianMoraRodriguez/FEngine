#pragma once

/**
 * @file ImageLoader.h
 * @brief Decodes image files (PNG, JPEG, ...) into raw RGBA8 pixels.
 */

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fe {

/// A decoded image: tightly packed 8-bit RGBA pixels, rows from top to bottom.
struct ImageData {
    uint32_t             width  = 0;
    uint32_t             height = 0;
    std::vector<uint8_t> pixels;   ///< width * height * 4 bytes.
};

/// A decoded high-dynamic-range image: linear float RGB, rows from top to bottom.
/// Unlike ImageData the values are not limited to 0..1: the sun in a sky
/// photograph can be thousands of times brighter than the blue next to it.
struct HdrImageData {
    uint32_t           width  = 0;
    uint32_t           height = 0;
    std::vector<float> pixels;   ///< width * height * 3 floats (R, G, B).
};

/**
 * @brief Where an image comes from: a file on disk, or the encoded bytes
 *        (PNG, JPEG...) of an image embedded in a model file.
 */
struct ImageSource {
    std::filesystem::path                          file;      ///< Used when @c encoded is empty.
    std::shared_ptr<const std::vector<uint8_t>>    encoded;   ///< Embedded file contents, still compressed.
    std::string                                    label;     ///< For log messages.
};

/**
 * @brief Reads and decodes an image file.
 *
 * Whatever the file stores (grey, grey+alpha, RGB, RGBA; 8 or 16 bits), the
 * result is always 4 channels of 8 bits, so the GPU side only has to deal
 * with one layout. Files without alpha get an opaque one.
 *
 * Safe to call from a background thread: it touches no global state.
 *
 * @param path Image file. Unicode paths are fine.
 * @return The pixels, or `std::nullopt` (with the reason logged) if the file
 *         is missing, unreadable, not an image, empty or too large.
 */
std::optional<ImageData> loadImage(const std::filesystem::path& path);

/**
 * @brief Decodes an image that is already in memory (the bytes of a PNG,
 *        JPEG... file). Same result and same thread safety as loadImage().
 * @param label Name shown in error messages.
 */
std::optional<ImageData> decodeImage(const uint8_t* bytes, size_t size, const std::string& label);

/**
 * @brief Reads an image as linear floating-point colour (for environment maps).
 *
 * Radiance `.hdr` files are read as they are. Any other format (PNG, JPEG...)
 * is converted from sRGB to linear, so an ordinary panorama works too, only
 * without the extra brightness. Safe to call from a background thread.
 *
 * @return The pixels, or `std::nullopt` (with the reason logged) on failure.
 */
std::optional<HdrImageData> loadImageHdr(const std::filesystem::path& path);

/**
 * @brief Shrinks an image so that neither side is larger than @p maxSide.
 *
 * Averages blocks of whole pixels (a box filter), so the picture is a clean
 * smaller copy; an image already small enough is returned unchanged.
 * Intended for previews, where a 4096-pixel texture has no use.
 */
ImageData shrunkToFit(ImageData image, uint32_t maxSide);

/// Decodes @p source, from memory or from its file.
std::optional<ImageData> loadImage(const ImageSource& source);

} // namespace fe
