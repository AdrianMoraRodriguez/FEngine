#pragma once

/**
 * @file AssetPaths.h
 * @brief The asset folder, and conversion between file paths and the
 *        relative text paths that scenes store.
 *
 * A scene must mean the same on every machine, so it never stores
 * `C:\Users\me\project\assets\wood.png`; it stores `textures/wood.png`,
 * relative to the asset folder. The editor sets the root once at startup.
 *
 * Main thread only: the root is plain global state.
 */

#include "core/PathUtil.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace fe::assets {

namespace detail {
inline std::filesystem::path& rootStorage() {
    static std::filesystem::path root;
    return root;
}
} // namespace detail

/// Sets the asset folder. Call once at startup.
inline void setRoot(std::filesystem::path root) { detail::rootStorage() = std::move(root); }

/// @return The asset folder (empty until setRoot() is called).
inline const std::filesystem::path& root() { return detail::rootStorage(); }

/**
 * @brief Converts a file path to a path relative to the asset folder.
 * @return UTF-8 text with '/' as separator, or an empty string if the file
 *         is not inside the asset folder.
 */
inline std::string toRelative(const std::filesystem::path& file) {
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(file, root(), ec);
    if (ec || relative.empty() || *relative.begin() == "..") return {};
    return toGenericUtf8(relative);
}

/// @return The full path of a relative asset path (see toRelative()).
inline std::filesystem::path resolve(const std::string& relative) {
    return root() / std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(relative.data()), relative.size()));
}

} // namespace fe::assets
