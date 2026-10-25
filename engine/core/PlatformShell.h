#pragma once

/**
 * @file PlatformShell.h
 * @brief Small requests to the operating system's own file manager.
 */

#include <filesystem>

namespace fe {

/**
 * @brief Shows @p path in the OS file manager.
 *
 * On Windows the file is selected in an Explorer window; on macOS in Finder;
 * elsewhere its folder is opened (a Linux file manager cannot be asked to
 * select a file in a standard way). A folder is simply opened.
 * Returns immediately; failures are logged, not reported.
 */
void revealInFileManager(const std::filesystem::path& path);

} // namespace fe
