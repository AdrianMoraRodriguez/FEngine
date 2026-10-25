#pragma once

/**
 * @file ImageLoader.h
 * @brief Decodes image files (PNG, JPEG, ...) into raw RGBA8 pixels.
 */

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace fe {

/// A decoded image: tightly packed 8-bit RGBA pixels, rows from top to bottom.
struct ImageData {
    uint32_t             width  = 0;
    uint32_t             height = 0;
    std::vector<uint8_t> pixels;   ///< width * height * 4 bytes.
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

} // namespace fe
