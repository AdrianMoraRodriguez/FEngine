/**
 * @file Version.h
 * @brief Engine version information.
 */

#pragma once

namespace fe {

/**
 * @brief Semantic version number of the engine.
 *
 * Follows [Semantic Versioning](https://semver.org): a change in @ref major
 * signals a breaking API change, @ref minor adds functionality in a
 * backwards-compatible way, and @ref patch is reserved for fixes.
 */
struct Version {
    int major;  ///< Incremented on breaking API changes.
    int minor;  ///< Incremented on backwards-compatible additions.
    int patch;  ///< Incremented on backwards-compatible fixes.
};

/**
 * @brief Returns the engine version as a structured value.
 *
 * Prefer this over parsing engineVersionString() when the individual
 * components are needed, for example to gate behaviour on a minimum version.
 *
 * @return The current engine version.
 *
 * @see engineVersionString()
 */
Version engineVersion();

/**
 * @brief Returns the engine version as a human-readable string.
 *
 * The format is `"FEngine <major>.<minor>.<patch>"`, intended for log output
 * and about dialogs rather than for programmatic comparison.
 *
 * @return A null-terminated string with static storage duration. The caller
 *         must not free it.
 *
 * @see engineVersion()
 */
const char* engineVersionString();

}
