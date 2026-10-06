#pragma once

/**
 * @file UUID.h
 * @brief Universally unique identifier for scene entities.
 */

#include <cstdint>

namespace fe {

/// @brief A 64-bit identifier unique within a session and across saved scenes.
///
/// Zero is reserved as the null UUID and is never returned by generateUUID().
using UUID = std::uint64_t;

/// @brief Generates a new non-zero UUID using a random 64-bit engine.
/// @return A UUID guaranteed not to be zero.
UUID generateUUID();

} // namespace fe
