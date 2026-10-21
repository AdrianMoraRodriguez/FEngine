#pragma once

/**
 * @file PathUtil.h
 * @brief Safe conversion of file system paths to text.
 */

#include <filesystem>
#include <string>

namespace fe {

/**
 * @brief Returns the UTF-8 text of a path. Never throws on a valid path.
 *
 * Do not use `path::string()` for names that come from the user's disk. On
 * Windows it converts to the system's ANSI code page and throws
 * std::system_error for any character that page cannot represent (CJK
 * characters, many accented letters in other languages, emoji...). One file
 * with such a name was enough to take the whole editor down. UTF-8 can
 * represent every character, and it is what ImGui and the log expect.
 */
inline std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

/// Like toUtf8(), but with '/' as the separator on every platform.
/// Use it where the text must be identical on every machine, for example
/// when hashing a path into an asset handle.
inline std::string toGenericUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

} // namespace fe
